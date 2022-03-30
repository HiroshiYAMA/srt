/*
 * SRT - Secure, Reliable, Transport
 * Copyright (c) 2018 Haivision Systems Inc.
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 */

// Just for formality. This file should be used
#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>
#include <memory>
#include <string>
#include <stdexcept>
#include <iterator>
#include <map>
#include <srt.h>
#if !defined(_WIN32)
#include <sys/ioctl.h>
#else
#include <fcntl.h>
#include <io.h>
#endif
#if defined(SUNOS)
#include <sys/filio.h>
#endif

#include "netinet_any.h"
#include "apputil.hpp"
#include "socketoptions.hpp"
#include "uriparser.hpp"
#include "transmitmedia.hpp"
#include "srt_compat.h"
#include "verbose.hpp"

#include <assert.h>
#include <algorithm>
#include <numeric>
#include <chrono>

#ifdef CHECK_SUM_MODE_CRC32
#include <crc.h>
#include <secblock.h>
#endif

using namespace std;

bool g_stats_are_printed_to_stdout = false;
bool transmit_total_stats = false;
unsigned long transmit_bw_report = 0;
unsigned long transmit_stats_report = 0;
unsigned long transmit_chunk_size = SRT_LIVE_MAX_PLSIZE;

volatile bool int_state_NDI = false;
volatile bool timer_state_NDI = false;

static uint64_t calc_checksum(const std::vector<uint8_t> data)
{
    uint64_t cs = 0;

    // auto st = std::chrono::steady_clock::now();
#ifndef CHECK_SUM_MODE_CRC32
    cs = std::accumulate(data.begin(), data.end(), 0);
#else
    CryptoPP::CRC32 crc;
    CryptoPP::SecByteBlock digest(crc.DigestSize());
    crc.Update(data.data(), data.size());
    crc.Final(digest);
    for (size_t i = 0; i < std::min(digest.size(), size_t(4)); i++) {
        cs = (cs << 8) + digest[i];
    }
#endif
    // auto et = std::chrono::steady_clock::now();
    // auto dt = std::chrono::duration_cast<std::chrono::nanoseconds>(et - st);
    // std::cerr << "TIME(CHECK SUM) = " << dt.count() / 1000.0f << "(usec)" << std::endl;

    return cs;
}

template<typename T, typename S> void ifsr(std::vector<T> &data, S &val, size_t &pos)
{
    if ((pos + sizeof(S)) > data.size()) return;

    S *p = reinterpret_cast<S *>(&data[pos]);
    val = *p;
    pos += sizeof(S);
};

template<typename T, typename S> void ofsw(std::vector<T> &data, S &val)
{
    T *p = reinterpret_cast<T *>(&val);
    for (size_t i = 0; i < sizeof(val); i++) {
        data.push_back(p[i]);
    }
};

enum class em_NDI_TYPE : int8_t
{
    Video,
    Audio,
    Metadata,
    GenLock,
    Tally,
    Unknown = -1
};

struct st_NDI_on_SRT_header
{
    static constexpr auto MARKER = "NDIonSRT";

    char marker[8];
    struct st_version
    {
        int8_t major;
        int16_t minor;
    } version;
    em_NDI_TYPE ndi_type;    // int8_t.
    uint32_t frame_count;
    uint8_t reserved[8];
    uint64_t ndi_data_size;
    uint64_t check_sum;

    static std::vector<uint8_t> pack(struct st_NDI_on_SRT_header &header)
    {
        std::vector<uint8_t> data;

        data.reserve(sizeof(st_NDI_on_SRT_header));
        data.clear();

        for (size_t i = 0; i < std::size(header.marker); i++) {
            ofsw(data, header.marker[i]);
        }
        ofsw(data, header.version.major);
        ofsw(data, header.version.minor);
        ofsw(data, header.ndi_type);
        ofsw(data, header.frame_count);
        for (size_t i = 0; i < std::size(header.reserved); i++) {
            ofsw(data, header.reserved[i]);
        }
        ofsw(data, header.ndi_data_size);
        ofsw(data, header.check_sum);

        return data;
    }

    static struct st_NDI_on_SRT_header unpack(std::vector<uint8_t> &data, size_t pos = 0)
    {
        st_NDI_on_SRT_header header;

        for (size_t i = 0; i < std::size(header.marker); i++) {
            ifsr(data, header.marker[i], pos);
        }
        ifsr(data, header.version.major, pos);
        ifsr(data, header.version.minor, pos);
        ifsr(data, header.ndi_type, pos);
        ifsr(data, header.frame_count, pos);
        for (size_t i = 0; i < std::size(header.reserved); i++) {
            ifsr(data, header.reserved[i], pos);
        }
        ifsr(data, header.ndi_data_size, pos);
        ifsr(data, header.check_sum, pos);

        return header;
    }
};

class NdiSource: public Source
{
    NDIlib_recv_create_v3_t recv_desc;
    NDIlib_recv_instance_t pNDI_recv = nullptr;

    NDIlib_video_frame_v2_t video_frame;
    NDIlib_audio_frame_v3_t audio_frame;
    NDIlib_metadata_frame_t metadata_frame;

    bool is_received = false;
    bool is_sent_header = false;
    st_NDI_on_SRT_header header;
    int idx_in = 0;
    int idx_out = 1;
    std::vector<uint8_t> ndi_data[2];
    size_t pos_to_send;

    uint32_t frame_count = 0;

    bool eof = false;

public:

    NdiSource(const string& machine_name, const string& ndi_source_name)
    {
        // MACHINE_NAME (NDI_SOURCE_NAME)
        auto idx = ndi_source_name.find("/");
        std::string str_sn;
        if (idx != string::npos) {
            str_sn = ndi_source_name.substr(idx + 1, ndi_source_name.size() - idx);
        } else {
            str_sn = ndi_source_name;
        }
        std::string ndi_name = machine_name + " " + "(" + str_sn + ")";

        NDIlib_source_t ndi_src;
        ndi_src.p_ndi_name = ndi_name.c_str();

        // We now have at least one source, so we create a receiver to look at it.
        recv_desc.source_to_connect_to = ndi_src;
        recv_desc.color_format = (NDIlib_recv_color_format_e)NDIlib_recv_color_format_compressed_v5;
        pNDI_recv = NDIlib_recv_create_v3(&recv_desc);
        if (!pNDI_recv) {
            throw std::runtime_error(ndi_name + ": Can't create & connect NDI recciver");
        }
    }

    ~NdiSource() override
    {
        for (auto i = 0; i < 2; i++) {
            ndi_data[i].clear();
            ndi_data[i].shrink_to_fit();
        }

        // Destroy the receiver
        NDIlib_recv_destroy(pNDI_recv);
    }

    int Read(size_t chunk, MediaPacket& pkt, ostream & ignored SRT_ATR_UNUSED = cout) override
    {
        // std::cerr << "NDIlib_recv_get_no_connections: " << NDIlib_recv_get_no_connections(pNDI_recv) << std::endl;

        while (!is_received && !int_state_NDI && !timer_state_NDI) {
            switch (NDIlib_recv_capture_v3(pNDI_recv, &video_frame, &audio_frame, &metadata_frame, 5)) {
            // No data
            case NDIlib_frame_type_none:
                // std::cerr << "[NDI receive] NDIlib_frame_type_none." << std::endl;
                break;

            // Video data
            case NDIlib_frame_type_video:
                // std::cerr << "[NDI receive] NDIlib_frame_type_video." << std::endl;
                {
                    auto &data = ndi_data[idx_in];
                    data.clear();
                    ofsw(data, video_frame.xres);
                    ofsw(data, video_frame.yres);
                    ofsw(data, video_frame.FourCC);
                    ofsw(data, video_frame.frame_rate_N);
                    ofsw(data, video_frame.frame_rate_D);
                    ofsw(data, video_frame.picture_aspect_ratio);
                    ofsw(data, video_frame.frame_format_type);
                    ofsw(data, video_frame.timecode);
                    ofsw(data, video_frame.data_size_in_bytes);
                    ofsw(data, video_frame.timestamp);

                    // metadata.
                    if (video_frame.p_metadata != nullptr) {
                        auto len = strlen(video_frame.p_metadata);
                        data.resize(data.size() + len);
                        memcpy(&data[data.size() - len], video_frame.p_metadata, len);
                    }
                    // add null charactor.
                    {
                        char c = '\0';
                        ofsw(data, c);
                    }

                    // data.
                    if (video_frame.p_data != nullptr) {
                        auto sz = video_frame.data_size_in_bytes;
                        data.resize(data.size() + sz);
                        memcpy(&data[data.size() - sz], video_frame.p_data, sz);
                    }

                    // Free the memory
                    NDIlib_recv_free_video_v2(pNDI_recv, &video_frame);

                    // update header.
                    memcpy(header.marker, st_NDI_on_SRT_header::MARKER, std::size(header.marker));
                    header.version.major = 0;
                    header.version.minor = 0;
                    header.ndi_type = em_NDI_TYPE::Video;
                    header.frame_count = frame_count; frame_count++;
                    memset(header.reserved, 0, std::size(header.reserved));
                    header.ndi_data_size = data.size();
                    uint64_t cs = calc_checksum(data);
                    header.check_sum = cs;

                    // update index.
                    idx_out = idx_in;
                    idx_in = (idx_in + 1) % 2;

                    is_received = true;
                    is_sent_header = false;
                    pos_to_send = 0;
                }
                break;

            // Audio data
            case NDIlib_frame_type_audio:
                // std::cerr << "[NDI receive] NDIlib_frame_type_audio." << std::endl;
                NDIlib_recv_free_audio_v3(pNDI_recv, &audio_frame);
                break;

            // Meta data
            case NDIlib_frame_type_metadata:
                // std::cerr << "[NDI receive] NDIlib_frame_type_metadata." << std::endl;
                NDIlib_recv_free_metadata(pNDI_recv, &metadata_frame);
                break;

            // There is a status change on the receiver (e.g. new web interface)
            case NDIlib_frame_type_status_change:
                // std::cerr << "[NDI receive] NDIlib_frame_type_status_change." << std::endl;
                std::cerr << "Receiver connection status changed." << std::endl;
                break;

            case NDIlib_frame_type_error:
                // std::cerr << "[NDI receive] NDIlib_frame_type_error." << std::endl;
                std::cerr << "Receiver connection lost." << std::endl;
                eof = true;
                break;

            // Everything else
            default:
                // std::cerr << "[NDI receive] Others." << std::endl;
                break;
            }
        }

        // copy payload.
        size_t sz;
        if (is_sent_header) {
            auto &data_size = header.ndi_data_size;

            if (pos_to_send + chunk <= data_size) {
                sz = chunk;
            } else {
                sz = data_size - pos_to_send;
            }
            pkt.payload.resize(sz);

            auto &data = ndi_data[idx_out];
            memcpy(pkt.payload.data(), &data[pos_to_send], sz);
            pos_to_send += sz;

            if (pos_to_send >= data_size) is_received = false;

        } else {
            auto packet = st_NDI_on_SRT_header::pack(header);

            sz = packet.size();
            assert(sz <= chunk);
            pkt.payload.resize(sz);

            memcpy(pkt.payload.data(), packet.data(), sz);

            is_sent_header = true;
        }

        pkt.time = srt_time_now();

        if (pkt.payload.empty())
        {
            return 0;
        }

        return (int)sz;
    }

    bool IsOpen() override { return (pNDI_recv != nullptr); }
    bool End() override { return eof; }
};

class NdiTarget: public Target
{
    enum class em_WRITE_STATE : int
    {
        NONE,
        RECEIVE_HEADER,
        RECEIVE_NDI_DATA,
        SEND_NDI,
    };

    NDIlib_send_create_t send_desc;
    NDIlib_send_instance_t pNDI_send = nullptr;

    NDIlib_video_frame_v2_t video_frame;
    NDIlib_audio_frame_v3_t audio_frame;
    NDIlib_metadata_frame_t metadata_frame;

    em_WRITE_STATE write_stat = em_WRITE_STATE::NONE;
    st_NDI_on_SRT_header header;
    int idx_in = 0;
    int idx_out = 1;
    std::vector<uint8_t> ndi_data[2];
    std::string metadata[2];

    uint32_t frame_count = 0;

    bool eof = false;

public:

    NdiTarget(const string& /* machine_name */, const string& ndi_source_name)
    {
        // MACHINE_NAME (NDI_SOURCE_NAME)
        auto idx = ndi_source_name.find("/");
        std::string str_sn;
        if (idx != string::npos) {
            str_sn = ndi_source_name.substr(idx + 1, ndi_source_name.size() - idx);
        } else {
            str_sn = ndi_source_name;
        }

        send_desc.clock_video = send_desc.clock_audio = false;
        send_desc.p_ndi_name = str_sn.c_str();

        // We create the NDI sender
        pNDI_send = NDIlib_send_create(&send_desc);
        if (!pNDI_send) {
            throw std::runtime_error(str_sn + ": Can't create & connect NDI sender");
        }
    }

    ~NdiTarget() override
    {
        for (auto i = 0; i < 2; i++) {
            ndi_data[i].clear();
            ndi_data[i].shrink_to_fit();

            metadata[i].clear();
            metadata[i].shrink_to_fit();
        }

        // Because one buffer is in flight we need to make sure that there is no chance that we might free it before
        // NDI is done with it. You can ensure this either by sending another frame, or just by sending a frame with
        // a NULL pointer.
        NDIlib_send_send_video_async_v2(pNDI_send, NULL);

        // Destroy the NDI sender
        NDIlib_send_destroy(pNDI_send);
    }

    int Write(const char* data, size_t size, int64_t time SRT_ATR_UNUSED, ostream & ignored SRT_ATR_UNUSED = cout) override
    {
        std::vector<uint8_t> vec;
        vec.resize(size);
        memcpy(vec.data(), data, size);

        bool is_header = (memcmp(vec.data(), st_NDI_on_SRT_header::MARKER, std::size(header.marker)) == 0);
        if (is_header) {
            if (write_stat != em_WRITE_STATE::NONE) {
                std::cerr << "======== RESET!! NDI send state to 'RECEIVE_HEADER'. ======== stat = " << (int)write_stat << std::endl;
            }
            write_stat = em_WRITE_STATE::RECEIVE_HEADER;
        }

        switch (write_stat) {
        case em_WRITE_STATE::RECEIVE_HEADER:
            header = st_NDI_on_SRT_header::unpack(vec);
            frame_count = header.frame_count;
            ndi_data[idx_in].clear();
            metadata[idx_in].clear();
            write_stat = em_WRITE_STATE::RECEIVE_NDI_DATA;
            break;

        case em_WRITE_STATE::RECEIVE_NDI_DATA:
            {
                auto &dst = ndi_data[idx_in];
                // dst.insert(dst.end(), vec);
                dst.resize(dst.size() + size);
                memcpy(&dst[dst.size() - size], vec.data(), size);

                if (dst.size() >= header.ndi_data_size) {
                    write_stat = em_WRITE_STATE::SEND_NDI;
                    idx_out = idx_in;
                    idx_in = (idx_in + 1) % 2;
                }
            }
            break;

        default:
            break;
        }

        if (write_stat == em_WRITE_STATE::SEND_NDI) {
            switch (header.ndi_type) {
            case em_NDI_TYPE::Video:
                {
                    size_t pos = 0;
                    auto &data = ndi_data[idx_out];
                    uint64_t cs = calc_checksum(data);
                    if (cs == header.check_sum) {
                        ifsr(data, video_frame.xres, pos);
                        ifsr(data, video_frame.yres, pos);
                        ifsr(data, video_frame.FourCC, pos);
                        ifsr(data, video_frame.frame_rate_N, pos);
                        ifsr(data, video_frame.frame_rate_D, pos);
                        ifsr(data, video_frame.picture_aspect_ratio, pos);
                        ifsr(data, video_frame.frame_format_type, pos);
                        ifsr(data, video_frame.timecode, pos);
                        ifsr(data, video_frame.data_size_in_bytes, pos);
                        ifsr(data, video_frame.timestamp, pos);

                        // copy metadata.
                        metadata[idx_out] = std::string((char *)&data[pos]);
                        pos += (metadata[idx_out].length() + 1);
                        video_frame.p_metadata = metadata[idx_out].data();

                        // copy data.
                        video_frame.p_data = &data[pos];
                        pos += video_frame.data_size_in_bytes;

                        // send.
                        NDIlib_send_send_video_async_v2(pNDI_send, &video_frame);
                    } else {
                        std::cerr << "CHECK SUM: " << cs << " : " << header.check_sum << " : " << std::boolalpha << (cs == header.check_sum) << std::endl;
                    }
                }
                break;

            case em_NDI_TYPE::Audio:
                break;

            case em_NDI_TYPE::Metadata:
                break;

            case em_NDI_TYPE::GenLock:
                break;

            case em_NDI_TYPE::Tally:
                break;

            default:
                break;
            }

            write_stat = em_WRITE_STATE::NONE;
        }

        return (pNDI_send && data ? size : 0);
    }

    bool IsOpen() override { return true; }
    bool Broken() override { return true; }
    void Close() override {}
};

template <class Iface> struct Ndi;
template <> struct Ndi<Source> { typedef NdiSource type; };
template <> struct Ndi<Target> { typedef NdiTarget type; };

template <class Iface>
Iface* CreateNdi(const string& machine_name, const string& ndi_source_name) { return new typename Ndi<Iface>::type (machine_name, ndi_source_name); }

class FileSource: public Source
{
    ifstream ifile;
    string filename_copy;
public:

    FileSource(const string& path): ifile(path, ios::in | ios::binary), filename_copy(path)
    {
        if ( !ifile )
            throw std::runtime_error(path + ": Can't open file for reading");
    }

    int Read(size_t chunk, MediaPacket& pkt, ostream & ignored SRT_ATR_UNUSED = cout) override
    {
        if (pkt.payload.size() < chunk)
            pkt.payload.resize(chunk);

        pkt.time = 0;
        ifile.read(pkt.payload.data(), chunk);
        size_t nread = ifile.gcount();
        if (nread < pkt.payload.size())
            pkt.payload.resize(nread);

        if (pkt.payload.empty())
        {
            return 0;
        }

        return (int) nread;
    }

    bool IsOpen() override { return bool(ifile); }
    bool End() override { return ifile.eof(); }
};

class FileTarget: public Target
{
    ofstream ofile;
public:

    FileTarget(const string& path): ofile(path, ios::out | ios::trunc | ios::binary) {}

    int Write(const char* data, size_t size, int64_t time SRT_ATR_UNUSED, ostream & ignored SRT_ATR_UNUSED = cout) override
    {
        ofile.write(data, size);
        return !(ofile.bad()) ? (int) size : 0;
    }

    bool IsOpen() override { return !!ofile; }
    bool Broken() override { return !ofile.good(); }
    //~FileTarget() { ofile.close(); }
    void Close() override { ofile.close(); }
};

template <class Iface> struct File;
template <> struct File<Source> { typedef FileSource type; };
template <> struct File<Target> { typedef FileTarget type; };

template <class Iface>
Iface* CreateFile(const string& name) { return new typename File<Iface>::type (name); }

shared_ptr<SrtStatsWriter> transmit_stats_writer;

void SrtCommon::InitParameters(string host, map<string,string> par)
{
    // Application-specific options: mode, blocking, timeout, adapter
    if (Verbose::on && !par.empty())
    {
        Verb() << "SRT parameters specified:\n";
        for (map<string,string>::iterator i = par.begin(); i != par.end(); ++i)
        {
            cerr << "\t" << i->first << " = '" << i->second << "'\n";
        }
    }

    string adapter;
    if (par.count("adapter"))
    {
        adapter = par.at("adapter");
    }

    m_mode = "default";
    if (par.count("mode"))
    {
        m_mode = par.at("mode");
    }
    SocketOption::Mode mode = SrtInterpretMode(m_mode, host, adapter);
    if (mode == SocketOption::FAILURE)
    {
        Error("Invalid mode");
    }

    // Fix the mode name after successful interpretation
    m_mode = SocketOption::mode_names[mode];

    par.erase("mode");

    if (par.count("timeout"))
    {
        m_timeout = stoi(par.at("timeout"), 0, 0);
        par.erase("timeout");
    }

    if (par.count("adapter"))
    {
        m_adapter = par.at("adapter");
        par.erase("adapter");
    }
    else if (m_mode == "listener")
    {
        // For listener mode, adapter is taken from host,
        // if 'adapter' parameter is not given
        m_adapter = host;
    }

    if (par.count("tsbpd") && false_names.count(par.at("tsbpd")))
    {
        m_tsbpdmode = false;
    }

    if (par.count("port"))
    {
        m_outgoing_port = stoi(par.at("port"), 0, 0);
        par.erase("port");
    }

    // That's kinda clumsy, but it must rely on the defaults.
    // Default mode is live, so check if the file mode was enforced
    if ((par.count("transtype") == 0 || par["transtype"] != "file")
        && transmit_chunk_size > SRT_LIVE_DEF_PLSIZE)
    {
        if (transmit_chunk_size > SRT_LIVE_MAX_PLSIZE)
            throw std::runtime_error("Chunk size in live mode exceeds 1456 bytes; this is not supported");

        par["payloadsize"] = Sprint(transmit_chunk_size);
    }

    // Assign the others here.
    m_options = par;
}

void SrtCommon::PrepareListener(string host, int port, int backlog)
{
    m_bindsock = srt_create_socket();
    if ( m_bindsock == SRT_ERROR )
        Error("srt_create_socket");

    int stat = ConfigurePre(m_bindsock);
    if ( stat == SRT_ERROR )
        Error("ConfigurePre");

    sockaddr_any sa = CreateAddr(host, port);
    sockaddr* psa = sa.get();
    Verb() << "Binding a server on " << host << ":" << port << " ...";

    stat = srt_bind(m_bindsock, psa, sizeof sa);
    if ( stat == SRT_ERROR )
    {
        srt_close(m_bindsock);
        Error("srt_bind");
    }

    Verb() << " listen...";

    stat = srt_listen(m_bindsock, backlog);
    if ( stat == SRT_ERROR )
    {
        srt_close(m_bindsock);
        Error("srt_listen");
    }
}

void SrtCommon::StealFrom(SrtCommon& src)
{
    // This is used when SrtCommon class designates a listener
    // object that is doing Accept in appropriate direction class.
    // The new object should get the accepted socket.
    m_output_direction = src.m_output_direction;
    m_timeout = src.m_timeout;
    m_tsbpdmode = src.m_tsbpdmode;
    m_options = src.m_options;
    m_bindsock = SRT_INVALID_SOCK; // no listener
    m_sock = src.m_sock;
    src.m_sock = SRT_INVALID_SOCK; // STEALING
}

bool SrtCommon::AcceptNewClient()
{
    sockaddr_any scl;
    Verb() << " accept... ";

    m_sock = srt_accept(m_bindsock, scl.get(), &scl.len);
    if ( m_sock == SRT_INVALID_SOCK )
    {
        srt_close(m_bindsock);
        m_bindsock = SRT_INVALID_SOCK;
        Error("srt_accept");
    }

    // we do one client connection at a time,
    // so close the listener.
    srt_close(m_bindsock);
    m_bindsock = SRT_INVALID_SOCK;

    Verb() << " connected.";

    // ConfigurePre is done on bindsock, so any possible Pre flags
    // are DERIVED by sock. ConfigurePost is done exclusively on sock.
    int stat = ConfigurePost(m_sock);
    if ( stat == SRT_ERROR )
        Error("ConfigurePost");

    return true;
}

void SrtCommon::Init(string host, int port, map<string,string> par, bool dir_output)
{
    m_output_direction = dir_output;
    InitParameters(host, par);

    Verb() << "Opening SRT " << (dir_output ? "target" : "source") << " " << m_mode
        << " on " << host << ":" << port;

    if ( m_mode == "caller" )
        OpenClient(host, port);
    else if ( m_mode == "listener" )
        OpenServer(m_adapter, port);
    else if ( m_mode == "rendezvous" )
        OpenRendezvous(m_adapter, host, port);
    else
    {
        throw std::invalid_argument("Invalid 'mode'. Use 'client' or 'server'");
    }
}

int SrtCommon::ConfigurePost(SRTSOCKET sock)
{
    bool no = false;
    int result = 0;
    if ( m_output_direction )
    {
        result = srt_setsockopt(sock, 0, SRTO_SNDSYN, &no, sizeof no);
        if ( result == -1 )
            return result;

        if ( m_timeout )
            return srt_setsockopt(sock, 0, SRTO_SNDTIMEO, &m_timeout, sizeof m_timeout);
    }
    else
    {
        result = srt_setsockopt(sock, 0, SRTO_RCVSYN, &no, sizeof no);
        if ( result == -1 )
            return result;

        if ( m_timeout )
            return srt_setsockopt(sock, 0, SRTO_RCVTIMEO, &m_timeout, sizeof m_timeout);
    }

    SrtConfigurePost(sock, m_options);

    for (const auto &o: srt_options)
    {
        if ( o.binding == SocketOption::POST && m_options.count(o.name) )
        {
            string value = m_options.at(o.name);
            bool ok = o.apply<SocketOption::SRT>(sock, value);
            if ( !ok )
                Verb() << "WARNING: failed to set '" << o.name << "' (post, "
                    << (m_output_direction? "target":"source") << ") to "
                    << value;
            else
                Verb() << "NOTE: SRT/post::" << o.name << "=" << value;
        }
    }

    return 0;
}

int SrtCommon::ConfigurePre(SRTSOCKET sock)
{
    int result = 0;

    bool no = false;
    if ( !m_tsbpdmode )
    {
        result = srt_setsockopt(sock, 0, SRTO_TSBPDMODE, &no, sizeof no);
        if ( result == -1 )
            return result;
    }

    result = srt_setsockopt(sock, 0, SRTO_RCVSYN, &no, sizeof no);
    if ( result == -1 )
        return result;


    // host is only checked for emptiness and depending on that the connection mode is selected.
    // Here we are not exactly interested with that information.
    vector<string> failures;

    // NOTE: here host = "", so the 'connmode' will be returned as LISTENER always,
    // but it doesn't matter here. We don't use 'connmode' for anything else than
    // checking for failures.
    SocketOption::Mode conmode = SrtConfigurePre(sock, "",  m_options, &failures);

    if ( conmode == SocketOption::FAILURE )
    {
        if ( Verbose::on )
        {
            cerr << "WARNING: failed to set options: ";
            copy(failures.begin(), failures.end(), ostream_iterator<string>(cerr, ", "));
            cerr << endl;
        }

        return SRT_ERROR;
    }

    return 0;
}

void SrtCommon::SetupAdapter(const string& host, int port)
{
    sockaddr_any localsa = CreateAddr(host, port);
    sockaddr* psa = localsa.get();
    int stat = srt_bind(m_sock, psa, sizeof localsa);
    if ( stat == SRT_ERROR )
        Error("srt_bind");
}

void SrtCommon::OpenClient(string host, int port)
{
    PrepareClient();

    if (m_outgoing_port || m_adapter != "")
    {
        SetupAdapter(m_adapter, m_outgoing_port);
    }

    ConnectClient(host, port);
}

void SrtCommon::PrepareClient()
{
    m_sock = srt_create_socket();
    if ( m_sock == SRT_ERROR )
        Error("srt_create_socket");

    int stat = ConfigurePre(m_sock);
    if ( stat == SRT_ERROR )
        Error("ConfigurePre");
}


void SrtCommon::ConnectClient(string host, int port)
{

    sockaddr_any sa = CreateAddr(host, port);
    sockaddr* psa = sa.get();

    Verb() << "Connecting to " << host << ":" << port;

    int stat = srt_connect(m_sock, psa, sizeof sa);
    if ( stat == SRT_ERROR )
    {
        srt_close(m_sock);
        Error("srt_connect");
    }

    stat = ConfigurePost(m_sock);
    if ( stat == SRT_ERROR )
        Error("ConfigurePost");
}

void SrtCommon::Error(string src)
{
    int errnov = 0;
    int result = srt_getlasterror(&errnov);
    string message = srt_getlasterror_str();
    Verb() << "\nERROR #" << result << "." << errnov << ": " << message;

    throw TransmissionError("error: " + src + ": " + message);
}

void SrtCommon::OpenRendezvous(string adapter, string host, int port)
{
    m_sock = srt_create_socket();
    if ( m_sock == SRT_ERROR )
        Error("srt_create_socket");

    bool yes = true;
    srt_setsockopt(m_sock, 0, SRTO_RENDEZVOUS, &yes, sizeof yes);

    int stat = ConfigurePre(m_sock);
    if ( stat == SRT_ERROR )
        Error("ConfigurePre");

    sockaddr_any sa = CreateAddr(host, port);
    if (sa.family() == AF_UNSPEC)
    {
        Error("OpenRendezvous: invalid target host specification: " + host);
    }

    const int outport = m_outgoing_port ? m_outgoing_port : port;

    sockaddr_any localsa = CreateAddr(adapter, outport, sa.family());

    Verb() << "Binding a server on " << adapter << ":" << outport;

    stat = srt_bind(m_sock, localsa.get(), sizeof localsa);
    if ( stat == SRT_ERROR )
    {
        srt_close(m_sock);
        Error("srt_bind");
    }

    Verb() << "Connecting to " << host << ":" << port;

    stat = srt_connect(m_sock, sa.get(), sizeof sa);
    if ( stat == SRT_ERROR )
    {
        srt_close(m_sock);
        Error("srt_connect");
    }

    stat = ConfigurePost(m_sock);
    if ( stat == SRT_ERROR )
        Error("ConfigurePost");
}

void SrtCommon::Close()
{
    Verb() << "SrtCommon: DESTROYING CONNECTION, closing sockets (rt%" << m_sock << " ls%" << m_bindsock << ")...";

    if ( m_sock != SRT_INVALID_SOCK )
    {
        srt_close(m_sock);
        m_sock = SRT_INVALID_SOCK;
    }

    if ( m_bindsock != SRT_INVALID_SOCK )
    {
        srt_close(m_bindsock);
        m_bindsock = SRT_INVALID_SOCK ;
    }

    Verb() << "SrtCommon: ... done.";
}

SrtCommon::~SrtCommon()
{
    Close();
}

SrtSource::SrtSource(string host, int port, const map<string,string>& par)
{
    Init(host, port, par, false);

    ostringstream os;
    os << host << ":" << port;
    hostport_copy = os.str();
}

int SrtSource::Read(size_t chunk, MediaPacket& pkt, ostream &out_stats)
{
    static unsigned long counter = 1;

    if (pkt.payload.size() < chunk)
        pkt.payload.resize(chunk);

    SRT_MSGCTRL ctrl;
    const int stat = srt_recvmsg2(m_sock, pkt.payload.data(), (int) chunk, &ctrl);
    if (stat <= 0)
    {
        pkt.payload.clear();
        return stat;
    }

    pkt.time = ctrl.srctime;

    chunk = size_t(stat);
    if (chunk < pkt.payload.size())
        pkt.payload.resize(chunk);

    const bool need_bw_report = transmit_bw_report && (counter % transmit_bw_report) == transmit_bw_report - 1;
    const bool need_stats_report = transmit_stats_report && (counter % transmit_stats_report) == transmit_stats_report - 1;

    if (need_bw_report || need_stats_report)
    {
        CBytePerfMon perf;
        srt_bstats(m_sock, &perf, need_stats_report && !transmit_total_stats);
        if (transmit_stats_writer != nullptr)
        {
            if (need_bw_report)
                cerr << transmit_stats_writer->WriteBandwidth(perf.mbpsBandwidth) << std::flush;
            if (need_stats_report)
                out_stats << transmit_stats_writer->WriteStats(m_sock, perf) << std::flush;
        }
    }
    ++counter;
    return stat;
}

int SrtTarget::ConfigurePre(SRTSOCKET sock)
{
    int result = SrtCommon::ConfigurePre(sock);
    if ( result == -1 )
        return result;

    int yes = 1;
    // This is for the HSv4 compatibility; if both parties are HSv5
    // (min. version 1.2.1), then this setting simply does nothing.
    // In HSv4 this setting is obligatory; otherwise the SRT handshake
    // extension will not be done at all.
    result = srt_setsockopt(sock, 0, SRTO_SENDER, &yes, sizeof yes);
    if ( result == -1 )
        return result;

    return 0;
}

int SrtTarget::Write(const char* data, size_t size, int64_t src_time, ostream &out_stats)
{
    static unsigned long counter = 1;

    SRT_MSGCTRL ctrl = srt_msgctrl_default;
    ctrl.srctime = src_time;
    int stat = srt_sendmsg2(m_sock, data, (int) size, &ctrl);
    if (stat == SRT_ERROR)
    {
        return stat;
    }

    const bool need_bw_report = transmit_bw_report && (counter % transmit_bw_report) == transmit_bw_report - 1;
    const bool need_stats_report = transmit_stats_report && (counter % transmit_stats_report) == transmit_stats_report - 1;

    if (need_bw_report || need_stats_report)
    {
        CBytePerfMon perf;
        srt_bstats(m_sock, &perf, need_stats_report && !transmit_total_stats);
        if (transmit_stats_writer != nullptr)
        {
            if (need_bw_report)
                cerr << transmit_stats_writer->WriteBandwidth(perf.mbpsBandwidth) << std::flush;
            if (need_stats_report)
                out_stats << transmit_stats_writer->WriteStats(m_sock, perf) << std::flush;
        }
    }
    ++counter;
    return stat;
}


SrtModel::SrtModel(string host, int port, map<string,string> par)
{
    InitParameters(host, par);
    if (m_mode == "caller")
        is_caller = true;
    else if (m_mode != "listener")
        throw std::invalid_argument("Only caller and listener modes supported");

    m_host = host;
    m_port = port;
}

void SrtModel::Establish(std::string& w_name)
{
    // This does connect or accept.
    // When this returned true, the caller should create
    // a new SrtSource or SrtTaget then call StealFrom(*this) on it.

    // If this is a connector and the peer doesn't have a corresponding
    // medium, it should send back a single byte with value 0. This means
    // that agent should stop connecting.

    if (is_caller)
    {
        // Establish a connection

        PrepareClient();

        if (w_name != "")
        {
            Verb() << "Connect with requesting stream [" << w_name << "]";
            srt::setstreamid(m_sock, w_name);
        }
        else
        {
            Verb() << "NO STREAM ID for SRT connection";
        }

        if (m_outgoing_port)
        {
            Verb() << "Setting outgoing port: " << m_outgoing_port;
            SetupAdapter("", m_outgoing_port);
        }

        ConnectClient(m_host, m_port);

        if (m_outgoing_port == 0)
        {
            // Must rely on a randomly selected one. Extract the port
            // so that it will be reused next time.
            sockaddr_any s(AF_INET);
            int namelen = s.size();
            if ( srt_getsockname(Socket(), s.get(), &namelen) == SRT_ERROR )
            {
                Error("srt_getsockname");
            }

            m_outgoing_port = s.hport();
            Verb() << "Extracted outgoing port: " << m_outgoing_port;
        }
    }
    else
    {
        // Listener - get a socket by accepting.
        // Check if the listener is already created first
        if (Listener() == SRT_INVALID_SOCK)
        {
            Verb() << "Setting up listener: port=" << m_port << " backlog=5";
            PrepareListener(m_adapter, m_port, 5);
        }

        Verb() << "Accepting a client...";
        AcceptNewClient();
        // This rewrites m_sock with a new SRT socket ("accepted" socket)
        w_name = srt::getstreamid(m_sock);
        Verb() << "... GOT CLIENT for stream [" << w_name << "]";
    }
}


template <class Iface> struct Srt;
template <> struct Srt<Source> { typedef SrtSource type; };
template <> struct Srt<Target> { typedef SrtTarget type; };

template <class Iface>
Iface* CreateSrt(const string& host, int port, const map<string,string>& par) { return new typename Srt<Iface>::type (host, port, par); }

class ConsoleSource: public Source
{
public:

    ConsoleSource()
    {
#ifdef _WIN32
        // The default stdin mode on windows is text.
        // We have to set it to the binary mode
        _setmode(_fileno(stdin), _O_BINARY);
#endif
    }

    int Read(size_t chunk, MediaPacket& pkt, ostream & ignored SRT_ATR_UNUSED = cout) override
    {
        if (pkt.payload.size() < chunk)
            pkt.payload.resize(chunk);

        bool st = cin.read(pkt.payload.data(), chunk).good();
        chunk = cin.gcount();
        if (chunk == 0 || !st)
        {
            pkt.payload.clear();
            return 0;
        }

        // Save this time to potentially use it for SRT target.
        pkt.time = srt_time_now();
        if (chunk < pkt.payload.size())
            pkt.payload.resize(chunk);

        return (int) chunk;
    }

    bool IsOpen() override { return cin.good(); }
    bool End() override { return cin.eof(); }
    int GetSysSocket() const override { return 0; };
};

class ConsoleTarget: public Target
{
public:

    ConsoleTarget()
    {
#ifdef _WIN32
        // The default stdout mode on windows is text.
        // We have to set it to the binary mode
        _setmode(_fileno(stdout), _O_BINARY);
#endif
    }

    virtual ~ConsoleTarget()
    {
        cout.flush();
    }

    int Write(const char* data, size_t len, int64_t src_time SRT_ATR_UNUSED, ostream & ignored SRT_ATR_UNUSED = cout) override
    {
        cout.write(data, len);
        return (int) len;
    }

    bool IsOpen() override { return cout.good(); }
    bool Broken() override { return cout.eof(); }
    int GetSysSocket() const override { return 0; };
};

template <class Iface> struct Console;
template <> struct Console<Source> { typedef ConsoleSource type; };
template <> struct Console<Target> { typedef ConsoleTarget type; };

template <class Iface>
Iface* CreateConsole() { return new typename Console<Iface>::type (); }


// More options can be added in future.
SocketOption udp_options [] {
    { "iptos", IPPROTO_IP, IP_TOS, SocketOption::PRE, SocketOption::INT, nullptr },
    // IP_TTL and IP_MULTICAST_TTL are handled separately by a common option, "ttl".
    { "mcloop", IPPROTO_IP, IP_MULTICAST_LOOP, SocketOption::PRE, SocketOption::INT, nullptr },
    { "sndbuf", SOL_SOCKET, SO_SNDBUF, SocketOption::PRE, SocketOption::INT, nullptr},
    { "rcvbuf", SOL_SOCKET, SO_RCVBUF, SocketOption::PRE, SocketOption::INT, nullptr}
};

static inline bool IsMulticast(in_addr adr)
{
    unsigned char* abytes = (unsigned char*)&adr.s_addr;
    unsigned char c = abytes[0];
    return c >= 224 && c <= 239;
}


class UdpCommon
{
protected:
    int m_sock = -1;
    sockaddr_any sadr;
    string adapter;
    map<string, string> m_options;

    void Setup(string host, int port, map<string,string> attr)
    {
        m_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (m_sock == -1)
            Error(SysError(), "UdpCommon::Setup: socket");

        int yes = 1;
        ::setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);

        // set non-blocking mode
#if defined(_WIN32)
        unsigned long ulyes = 1;
        if (ioctlsocket(m_sock, FIONBIO, &ulyes) == SOCKET_ERROR)
#else
        if (ioctl(m_sock, FIONBIO, (const char *)&yes) < 0)
#endif
        {
            Error(SysError(), "UdpCommon::Setup: ioctl FIONBIO");
        }

        sadr = CreateAddr(host, port);

        bool is_multicast = false;

        if (attr.count("multicast"))
        {
            // XXX: Here provide support for IPv6 multicast #1479
            if (sadr.family() != AF_INET)
            {
                throw std::runtime_error("UdpCommon: Multicast on IPv6 is not yet supported");
            }

            if (!IsMulticast(sadr.sin.sin_addr))
            {
                throw std::runtime_error("UdpCommon: requested multicast for a non-multicast-type IP address");
            }
            is_multicast = true;
        }
        else if (sadr.family() == AF_INET && IsMulticast(sadr.sin.sin_addr))
        {
            is_multicast = true;
        }

        if (is_multicast)
        {
            ip_mreq_source mreq_ssm;
            ip_mreq mreq;
            sockaddr_any maddr (AF_INET);
            int opt_name;
            void* mreq_arg_ptr;
            socklen_t mreq_arg_size;

            adapter = attr.count("adapter") ? attr.at("adapter") : string();
            if ( adapter == "" )
            {
                Verb() << "Multicast: home address: INADDR_ANY:" << port;
                maddr.sin.sin_family = AF_INET;
                maddr.sin.sin_addr.s_addr = htonl(INADDR_ANY);
                maddr.sin.sin_port = htons(port); // necessary for temporary use
            }
            else
            {
                Verb() << "Multicast: home address: " << adapter << ":" << port;
                maddr = CreateAddr(adapter, port);
            }

            if (attr.count("source"))
            {
#ifdef IP_ADD_SOURCE_MEMBERSHIP
                /* this is an ssm.  we need to use the right struct and opt */
                opt_name = IP_ADD_SOURCE_MEMBERSHIP;
                mreq_ssm.imr_multiaddr.s_addr = sadr.sin.sin_addr.s_addr;
                mreq_ssm.imr_interface.s_addr = maddr.sin.sin_addr.s_addr;
                inet_pton(AF_INET, attr.at("source").c_str(), &mreq_ssm.imr_sourceaddr);
                mreq_arg_size = sizeof(mreq_ssm);
                mreq_arg_ptr = &mreq_ssm;
#else
                throw std::runtime_error("UdpCommon: source-filter multicast not supported by OS");
#endif
            }
            else
            {
                opt_name = IP_ADD_MEMBERSHIP;
                mreq.imr_multiaddr.s_addr = sadr.sin.sin_addr.s_addr;
                mreq.imr_interface.s_addr = maddr.sin.sin_addr.s_addr;
                mreq_arg_size = sizeof(mreq);
                mreq_arg_ptr = &mreq;
            }

#ifdef _WIN32
            const char* mreq_arg = (const char*)mreq_arg_ptr;
            const auto status_error = SOCKET_ERROR;
#else
            const void* mreq_arg = mreq_arg_ptr;
            const auto status_error = -1;
#endif

#if defined(_WIN32) || defined(__CYGWIN__)
            // On Windows it somehow doesn't work when bind()
            // is called with multicast address. Write the address
            // that designates the network device here.
            // Also, sets port sharing when working with multicast
            sadr = maddr;
            int reuse = 1;
            int shareAddrRes = setsockopt(m_sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
            if (shareAddrRes == status_error)
            {
                throw runtime_error("marking socket for shared use failed");
            }
            Verb() << "Multicast(Windows): will bind to home address";
#else
            Verb() << "Multicast(POSIX): will bind to IGMP address: " << host;
#endif
            int res = setsockopt(m_sock, IPPROTO_IP, opt_name, mreq_arg, mreq_arg_size);

            if ( res == status_error )
            {
                Error(errno, "adding to multicast membership failed");
            }

            attr.erase("multicast");
            attr.erase("adapter");
        }

        // The "ttl" options is handled separately, it maps to both IP_TTL
        // and IP_MULTICAST_TTL so that TTL setting works for both uni- and multicast.
        if (attr.count("ttl"))
        {
            int ttl = stoi(attr.at("ttl"));
            int res = setsockopt(m_sock, IPPROTO_IP, IP_TTL, (const char*)&ttl, sizeof ttl);
            if (res == -1)
                Verb() << "WARNING: failed to set 'ttl' (IP_TTL) to " << ttl;
            res = setsockopt(m_sock, IPPROTO_IP, IP_MULTICAST_TTL, (const char*)&ttl, sizeof ttl);
            if (res == -1)
                Verb() << "WARNING: failed to set 'ttl' (IP_MULTICAST_TTL) to " << ttl;

            attr.erase("ttl");
        }

        m_options = attr;

        for (auto o: udp_options)
        {
            // Ignore "binding" - for UDP there are no post options.
            if ( m_options.count(o.name) )
            {
                string value = m_options.at(o.name);
                bool ok = o.apply<SocketOption::SYSTEM>(m_sock, value);
                if ( !ok )
                    Verb() << "WARNING: failed to set '" << o.name << "' to " << value;
            }
        }
    }

    void Error(int err, string src)
    {
        char buf[512];
        string message = SysStrError(err, buf, 512u);

        cerr << "\nERROR #" << err << ": " << message << endl;

        throw TransmissionError("error: " + src + ": " + message);
    }

    ~UdpCommon()
    {
#ifdef _WIN32
        if (m_sock != -1)
        {
           shutdown(m_sock, SD_BOTH);
           closesocket(m_sock);
           m_sock = -1;
        }
#else
        close(m_sock);
#endif
    }
};


class UdpSource: public Source, public UdpCommon
{
    bool eof = true;
public:

    UdpSource(string host, int port, const map<string,string>& attr)
    {
        Setup(host, port, attr);
        int stat = ::bind(m_sock, sadr.get(), sadr.size());
        if ( stat == -1 )
            Error(SysError(), "Binding address for UDP");
        eof = false;
    }

    int Read(size_t chunk, MediaPacket& pkt, ostream & ignored SRT_ATR_UNUSED = cout) override
    {
        if (pkt.payload.size() < chunk)
            pkt.payload.resize(chunk);

        sockaddr_any sa(sadr.family());
        socklen_t si = sa.size();
        int stat = recvfrom(m_sock, pkt.payload.data(), (int) chunk, 0, sa.get(), &si);
        if (stat < 1)
        {
            if (SysError() != EWOULDBLOCK)
                eof = true;
            pkt.payload.clear();
            return stat;
        }
        sa.len = si;

        // Save this time to potentially use it for SRT target.
        pkt.time = srt_time_now();
        chunk = size_t(stat);
        if (chunk < pkt.payload.size())
            pkt.payload.resize(chunk);

        return stat;
    }

    bool IsOpen() override { return m_sock != -1; }
    bool End() override { return eof; }

    int GetSysSocket() const override { return m_sock; };
};

class UdpTarget: public Target, public UdpCommon
{
public:
    UdpTarget(string host, int port, const map<string,string>& attr )
    {
        if (host.empty())
            cerr << "\nWARN Host for UDP target is not provided. Will send to localhost:" << port << ".\n";

        Setup(host, port, attr);
        if (adapter != "")
        {
            sockaddr_any maddr = CreateAddr(adapter, 0);
            if (maddr.family() != AF_INET)
            {
                Error(0, "UDP/target: IPv6 multicast not supported in the application");
            }

            in_addr addr = maddr.sin.sin_addr;

            int res = setsockopt(m_sock, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<const char*>(&addr), sizeof(addr));
            if (res == -1)
            {
                Error(SysError(), "setsockopt/IP_MULTICAST_IF: " + adapter);
            }
        }

    }

    int Write(const char* data, size_t len, int64_t src_time SRT_ATR_UNUSED,  ostream & ignored SRT_ATR_UNUSED = cout) override
    {
        int stat = sendto(m_sock, data, (int) len, 0, sadr.get(), sadr.size());
        if ( stat == -1 )
        {
            if ((false))
                Error(SysError(), "UDP Write/sendto");
            return stat;
        }
        return stat;
    }

    bool IsOpen() override { return m_sock != -1; }
    bool Broken() override { return false; }

    int GetSysSocket() const override { return m_sock; };
};

template <class Iface> struct Udp;
template <> struct Udp<Source> { typedef UdpSource type; };
template <> struct Udp<Target> { typedef UdpTarget type; };

template <class Iface>
Iface* CreateUdp(const string& host, int port, const map<string,string>& par) { return new typename Udp<Iface>::type (host, port, par); }

template<class Base>
inline bool IsOutput() { return false; }

template<>
inline bool IsOutput<Target>() { return true; }

template <class Base>
extern unique_ptr<Base> CreateMedium(const string& uri)
{
    unique_ptr<Base> ptr;

    UriParser u(uri);

    int iport = 0;
    switch ( u.type() )
    {
    default:
        break; // do nothing, return nullptr
    case UriParser::FILE:
        if (u.host() == "con" || u.host() == "console")
        {
            if (IsOutput<Base>() && (
                (Verbose::on && Verbose::cverb == &cout)
                || g_stats_are_printed_to_stdout))
            {
                cerr << "ERROR: file://con with -v or -r or -s would result in mixing the data and text info.\n";
                cerr << "ERROR: HINT: you can stream through a FIFO (named pipe)\n";
                throw invalid_argument("incorrect parameter combination");
            }
            ptr.reset(CreateConsole<Base>());
        }
// Disable regular file support for the moment
#if 0
        else
            ptr.reset( CreateFile<Base>(u.path()));
#endif
        break;

    case UriParser::NDI:
        if (u.path() == "") {
            std::cerr << "Path name is empty." << std::endl;
            throw invalid_argument("Invalid port number");
        }
        ptr.reset(CreateNdi<Base>(u.host(), u.path()));
        break;

    case UriParser::SRT:
        iport = atoi(u.port().c_str());
        if ( iport < 1024 )
        {
            cerr << "Port value invalid: " << iport << " - must be >=1024\n";
            throw invalid_argument("Invalid port number");
        }
        ptr.reset( CreateSrt<Base>(u.host(), iport, u.parameters()) );
        break;


    case UriParser::UDP:
        iport = atoi(u.port().c_str());
        if ( iport < 1024 )
        {
            cerr << "Port value invalid: " << iport << " - must be >=1024\n";
            throw invalid_argument("Invalid port number");
        }
        ptr.reset( CreateUdp<Base>(u.host(), iport, u.parameters()) );
        break;

    }

    if (ptr.get())
        ptr->uri = move(u);

    return ptr;
}


std::unique_ptr<Source> Source::Create(const std::string& url)
{
    return CreateMedium<Source>(url);
}

std::unique_ptr<Target> Target::Create(const std::string& url)
{
    return CreateMedium<Target>(url);
}
