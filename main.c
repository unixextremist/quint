#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>

#define MAX_HIST 100

typedef struct {
    int id;
    char user[32];
    char text[512];
} Msg;

Msg msgs[MAX_HIST];
int msg_cnt = 0;
int next_id = 1;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

const char *PAGE =
"<!DOCTYPE html>"
"<html>"
"<head>"
"<meta charset=\"utf-8\">"
"<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
"<title>quint</title>"
"<style>"
":root{--bg:#fff;--fg:#111;--a:#06f;--b:#ccc}"
".dark{--bg:#111;--fg:#eee;--a:#0af;--b:#444}"
"body{margin:0;font-family:system-ui,sans-serif;background:var(--bg);color:var(--fg);display:flex;flex-direction:column;height:100vh}"
"header{padding:12px 20px;border-bottom:1px solid var(--b);display:flex;justify-content:space-between;align-items:center}"
"#m{flex:1;overflow:auto;padding:20px}"
".msg{margin-bottom:10px}"
".msg b{color:var(--a)}"
"#f{display:flex;padding:20px;gap:10px;border-top:1px solid var(--b)}"
"input,button{background:var(--bg);color:var(--fg);border:1px solid var(--b);padding:10px;font:inherit;border-radius:4px}"
"input{flex:1}"
"button{cursor:pointer}"
"button:hover{border-color:var(--a)}"
"</style>"
"</head>"
"<body>"
"<header><b>quint</b><button onclick=\"t()\">&#9680;</button></header>"
"<div id=\"m\"></div>"
"<div id=\"f\">"
"<input id=\"u\" placeholder=\"name\" style=\"width:100px\">"
"<input id=\"t\" placeholder=\"message\" onkeydown=\"if(event.key==='Enter')s()\">"
"<button onclick=\"s()\">send</button>"
"</div>"
"<script>"
"if(localStorage.m==='1')document.body.classList.add('dark');"
"function t(){document.body.classList.toggle('dark');localStorage.m=document.body.classList.contains('dark')?'1':'0';}"
"let last=0;"
"async function l(){"
"const r=await fetch('/m?since='+last);"
"const j=await r.json();"
"j.forEach(x=>{last=x.i;const d=document.createElement('div');d.className='msg';d.innerHTML='<b>'+x.u+'</b> '+x.t;m.appendChild(d);m.scrollTop=m.scrollHeight;});"
"}"
"async function s(){"
"if(!u.value||!t.value)return;"
"await fetch('/s',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'u='+encodeURIComponent(u.value)+'&t='+encodeURIComponent(t.value)});"
"t.value='';l();"
"}"
"setInterval(l,1000);l();"
"</script>"
"</body>"
"</html>";

void get_ip(char *buf, size_t len) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) { strncpy(buf, "0.0.0.0", len); return; }
    struct sockaddr_in s;
    memset(&s, 0, sizeof(s));
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = inet_addr("8.8.8.8");
    s.sin_port = htons(53);
    if (connect(sock, (struct sockaddr*)&s, sizeof(s)) < 0) {
        close(sock); strncpy(buf, "0.0.0.0", len); return;
    }
    struct sockaddr_in n;
    socklen_t nl = sizeof(n);
    if (getsockname(sock, (struct sockaddr*)&n, &nl) < 0) {
        close(sock); strncpy(buf, "0.0.0.0", len); return;
    }
    inet_ntop(AF_INET, &n.sin_addr, buf, len);
    close(sock);
}

void url_decode(const char *src, char *dst, size_t n) {
    size_t i = 0, j = 0;
    while (src[i] && j < n - 1) {
        if (src[i] == '%' && src[i+1] && src[i+2]) {
            char h[3] = {src[i+1], src[i+2], 0};
            dst[j] = (char)strtol(h, NULL, 16);
            i += 3; j++;
        } else if (src[i] == '+') {
            dst[j] = ' '; i++; j++;
        } else {
            dst[j] = src[i]; i++; j++;
        }
    }
    dst[j] = 0;
}

void sanitize(char *s) {
    for (int i = 0; s[i]; i++)
        if (s[i] == '\"' || s[i] == '\\' || s[i] == '\n' || s[i] == '\r')
            s[i] = ' ';
}

void handle_client(int c) {
    char buf[4096];
    int n = recv(c, buf, sizeof(buf)-1, 0);
    if (n <= 0) return;
    buf[n] = 0;

    char method[8] = {0}, path[256] = {0};
    sscanf(buf, "%7s %255s", method, path);

    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        char h[256];
        int plen = strlen(PAGE);
        snprintf(h, sizeof(h), "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", plen);
        send(c, h, strlen(h), 0);
        send(c, PAGE, plen, 0);
    }
    else if (strcmp(method, "GET") == 0 && strncmp(path, "/m", 2) == 0) {
        int since = 0;
        sscanf(path, "/m?since=%d", &since);

        pthread_mutex_lock(&lock);
        char json[8192] = "[";
        int first = 1;
        for (int i = 0; i < msg_cnt; i++) {
            if (msgs[i].id > since) {
                if (!first) strcat(json, ",");
                first = 0;
                char item[1024];
                snprintf(item, sizeof(item), "{\"i\":%d,\"u\":\"%s\",\"t\":\"%s\"}", msgs[i].id, msgs[i].user, msgs[i].text);
                if (strlen(json) + strlen(item) < sizeof(json) - 4) strcat(json, item);
            }
        }
        strcat(json, "]");
        pthread_mutex_unlock(&lock);

        char h[256];
        int jlen = strlen(json);
        snprintf(h, sizeof(h), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", jlen);
        send(c, h, strlen(h), 0);
        send(c, json, jlen, 0);
    }
    else if (strcmp(method, "POST") == 0 && strcmp(path, "/s") == 0) {
        int cl = 0;
        char *clh = strstr(buf, "Content-Length: ");
        if (clh) sscanf(clh, "Content-Length: %d", &cl);

        char *body = strstr(buf, "\r\n\r\n");
        if (body) body += 4;

        int bl = body ? (int)strlen(body) : 0;
        if (body && cl > bl && cl < (int)sizeof(buf) - n - 1) {
            int r = recv(c, buf + n, cl - bl, 0);
            if (r > 0) { n += r; buf[n] = 0; body = strstr(buf, "\r\n\r\n"); if (body) body += 4; }
        }

        if (body && cl > 0) {
            char ue[64] = {0}, te[1024] = {0};
            char *up = strstr(body, "u="), *tp = strstr(body, "t=");
            if (up && tp) {
                up += 2; tp += 2;
                char *uep = strchr(up, '&');
                if (uep) { int l = uep - up; if (l >= (int)sizeof(ue)) l = sizeof(ue)-1; strncpy(ue, up, l); ue[l] = 0; }
                else strncpy(ue, up, sizeof(ue)-1);

                char *tep = strchr(tp, '&');
                if (tep) { int l = tep - tp; if (l >= (int)sizeof(te)) l = sizeof(te)-1; strncpy(te, tp, l); te[l] = 0; }
                else strncpy(te, tp, sizeof(te)-1);

                char u[32] = {0}, t[512] = {0};
                url_decode(ue, u, sizeof(u));
                url_decode(te, t, sizeof(t));
                sanitize(u); sanitize(t);

                if (strlen(u) && strlen(t)) {
                    pthread_mutex_lock(&lock);
                    if (msg_cnt < MAX_HIST) {
                        msgs[msg_cnt].id = next_id++;
                        strncpy(msgs[msg_cnt].user, u, 31);
                        strncpy(msgs[msg_cnt].text, t, 511);
                        msg_cnt++;
                    } else {
                        for (int i = 1; i < MAX_HIST; i++) msgs[i-1] = msgs[i];
                        msgs[MAX_HIST-1].id = next_id++;
                        strncpy(msgs[MAX_HIST-1].user, u, 31);
                        strncpy(msgs[MAX_HIST-1].text, t, 511);
                    }
                    pthread_mutex_unlock(&lock);
                }
            }
        }
        char *r = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(c, r, strlen(r), 0);
    }
    else {
        char *r = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        send(c, r, strlen(r), 0);
    }
}

void *thread(void *a) {
    int c = *(int*)a;
    free(a);
    handle_client(c);
    close(c);
    return NULL;
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);

    int port = 80;
    if (argc >= 3 && strcmp(argv[1], "-p") == 0) port = atoi(argv[2]);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    int o = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &o, sizeof(o));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = INADDR_ANY;
    a.sin_port = htons(port);

    if (bind(s, (struct sockaddr*)&a, sizeof(a)) < 0) { perror("bind"); return 1; }
    if (listen(s, 64) < 0) { perror("listen"); return 1; }

    char ip[32];
    get_ip(ip, sizeof(ip));
    printf("%s:%d\n", ip, port);
    fflush(stdout);

    while (1) {
        struct sockaddr_in ca;
        socklen_t cl = sizeof(ca);
        int c = accept(s, (struct sockaddr*)&ca, &cl);
        if (c < 0) continue;
        int *p = malloc(sizeof(int));
        if (!p) { close(c); continue; }
        *p = c;
        pthread_t tid;
        if (pthread_create(&tid, NULL, thread, p) != 0) { free(p); close(c); continue; }
        pthread_detach(tid);
    }
    close(s);
    return 0;
}
