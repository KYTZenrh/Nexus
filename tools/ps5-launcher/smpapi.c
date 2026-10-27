// Dev helper payload: sends one request to ShadowMountPlus's local API (127.0.0.1:10101, which
// is not reachable from the LAN) and saves the response to /data/smp_api.txt.
#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef API_PATH
#define API_PATH "/api/v1/games"
#endif
#ifndef API_BODY
#define API_BODY "{}"
#endif

int main(void) {
    FILE* out = fopen("/data/smp_api.txt", "w");
    if (!out) return 1;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons(10101);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr*)&a, sizeof a) != 0) {
        fprintf(out, "connect failed\n");
        fclose(out);
        return 1;
    }
    char req[1024];
    int n = snprintf(req, sizeof req,
                     "POST %s HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                     "Content-Length: %zu\r\nConnection: close\r\n\r\n%s",
                     API_PATH, strlen(API_BODY), API_BODY);
    send(s, req, n, 0);
    char buf[4096];
    ssize_t r;
    while ((r = recv(s, buf, sizeof buf, 0)) > 0) fwrite(buf, 1, (size_t)r, out);
    close(s);
    fclose(out);
    return 0;
}
