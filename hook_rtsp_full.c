#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <pthread.h>

#define MAX_CLIENT 4
#define RTP_PORT 8554
#define MTU 1400

typedef struct {
    void *virAddr;
    unsigned int length;
} IMPEncoderPack;

typedef struct {
    IMPEncoderPack *pack;
    unsigned int packCount;
} IMPEncoderStream;

typedef int (*orig_func_type)(int, IMPEncoderStream*, int);

static int clients[MAX_CLIENT];
static int server_sock = -1;

static unsigned short seq = 0;
static unsigned int timestamp = 0;

static unsigned char sps[128], pps[128];
static int sps_len = 0, pps_len = 0;

pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void send_all(unsigned char *data, int len) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_CLIENT; i++) {
        if (clients[i] > 0) {
            send(clients[i], data, len, 0);
        }
    }
    pthread_mutex_unlock(&lock);
}

void send_rtp_packet(unsigned char *data, int len, int marker) {
    unsigned char packet[1500];

    packet[0] = 0x80;
    packet[1] = 96 | (marker ? 0x80 : 0);
    *(unsigned short*)(packet+2) = htons(seq++);
    *(unsigned int*)(packet+4) = htonl(timestamp);
    *(unsigned int*)(packet+8) = htonl(0x12345678);

    memcpy(packet+12, data, len);
    send_all(packet, len + 12);
}

void send_nal(unsigned char *nal, int len) {
    if (len < MTU) {
        send_rtp_packet(nal, len, 1);
    } else {
        unsigned char fu_ind = (nal[0] & 0xE0) | 28;
        unsigned char fu_hdr_start = 0x80 | (nal[0] & 0x1F);
        unsigned char fu_hdr_mid = (nal[0] & 0x1F);
        unsigned char fu_hdr_end = 0x40 | (nal[0] & 0x1F);

        int pos = 1;
        while (pos < len) {
            int size = (len - pos > MTU-2) ? MTU-2 : (len - pos);

            unsigned char buf[1500];
            buf[0] = fu_ind;

            if (pos == 1) buf[1] = fu_hdr_start;
            else if (pos + size >= len) buf[1] = fu_hdr_end;
            else buf[1] = fu_hdr_mid;

            memcpy(buf+2, nal+pos, size);
            send_rtp_packet(buf, size+2, (pos+size>=len));

            pos += size;
        }
    }
}

void process_frame(unsigned char *data, int len) {
    int type = data[0] & 0x1F;

    if (type == 7) { memcpy(sps, data, len); sps_len = len; }
    if (type == 8) { memcpy(pps, data, len); pps_len = len; }

    send_nal(data, len);
}

char* base64_encode(const unsigned char *data, int len) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    static char out[256];
    int i, j = 0;
    for (i = 0; i < len; i += 3) {
        int v = data[i];
        v = (i+1<len) ? v<<8 | data[i+1] : v<<8;
        v = (i+2<len) ? v<<8 | data[i+2] : v<<8;

        out[j++] = tbl[(v>>18)&63];
        out[j++] = tbl[(v>>12)&63];
        out[j++] = (i+1<len)?tbl[(v>>6)&63]:'=';
        out[j++] = (i+2<len)?tbl[v&63]:'=';
    }
    out[j] = 0;
    return out;
}

void handle_client(int sock) {
    char buf[1024] = {0};
    recv(sock, buf, sizeof(buf), 0);

    if (strstr(buf, "OPTIONS")) {
        char *res = "RTSP/1.0 200 OK\r\nCSeq: 1\r\nPublic: OPTIONS,DESCRIBE,SETUP,PLAY\r\n\r\n";
        send(sock, res, strlen(res), 0);
    }

    else if (strstr(buf, "DESCRIBE")) {
        char sdp[512];
        sprintf(sdp,
        "v=0\r\n"
        "o=- 0 0 IN IP4 127.0.0.1\r\n"
        "s=H264\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP 96\r\n"
        "a=rtpmap:96 H264/90000\r\n"
        "a=fmtp:96 packetization-mode=1;sprop-parameter-sets=%s,%s\r\n",
        base64_encode(sps,sps_len), base64_encode(pps,pps_len));

        char res[1024];
        sprintf(res,
        "RTSP/1.0 200 OK\r\nCSeq: 2\r\nContent-Type: application/sdp\r\nContent-Length: %ld\r\n\r\n%s",
        strlen(sdp), sdp);

        send(sock, res, strlen(res), 0);
    }

    else if (strstr(buf, "SETUP")) {
        char *res = "RTSP/1.0 200 OK\r\nCSeq: 3\r\nTransport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n\r\n";
        send(sock, res, strlen(res), 0);
    }

    else if (strstr(buf, "PLAY")) {
        char *res = "RTSP/1.0 200 OK\r\nCSeq: 4\r\n\r\n";
        send(sock, res, strlen(res), 0);

        pthread_mutex_lock(&lock);
        for (int i=0;i<MAX_CLIENT;i++){
            if (clients[i]==0){
                clients[i]=sock;
                break;
            }
        }
        pthread_mutex_unlock(&lock);
    }
}

void* rtsp_server(void *arg) {
    struct sockaddr_in addr;

    server_sock = socket(AF_INET, SOCK_STREAM, 0);

    addr.sin_family = AF_INET;
    addr.sin_port = htons(8554);
    addr.sin_addr.s_addr = INADDR_ANY;

    bind(server_sock, (struct sockaddr*)&addr, sizeof(addr));
    listen(server_sock, 5);

    printf("RTSP READY: rtsp://IP:8554/live\n");

    while (1) {
        int sock = accept(server_sock, NULL, NULL);
        handle_client(sock);
    }
}

int IMP_Encoder_GetStream(int chn, IMPEncoderStream *stream, int block) {
    static orig_func_type orig = NULL;

    if (!orig) {
        orig = (orig_func_type)dlsym(RTLD_NEXT, "IMP_Encoder_GetStream");
        pthread_t tid;
        pthread_create(&tid, NULL, rtsp_server, NULL);
    }

    int ret = orig(chn, stream, block);

    if (ret == 0 && stream && stream->pack) {
        for (unsigned int i = 0; i < stream->packCount; i++) {
            process_frame(stream->pack[i].virAddr, stream->pack[i].length);
        }
        timestamp += 3600;
    }

    return ret;
}