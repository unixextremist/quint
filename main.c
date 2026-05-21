#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <signal.h>
#include <ifaddrs.h>

#define MAX_HIST 20
#define MAX_USER 32
#define MAX_MSG  65536

typedef struct {
    int id;
    char user[MAX_USER];
    char text[MAX_MSG];
} Msg;

Msg msgs[MAX_HIST];
int msg_cnt = 0;
int next_id = 1;
pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static char json_buf[2 * 1024 * 1024];

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
".msg{margin-bottom:12px;word-break:break-word}"
".msg b{color:var(--a)}"
"pre{background:var(--b);padding:10px;border-radius:4px;overflow:auto;margin:4px 0;white-space:pre-wrap}"
"code{font-family:monospace;font-size:0.9em}"
"img{max-width:100%;max-height:300px;border-radius:4px;display:block;margin-top:4px}"
"#f{display:flex;padding:20px;gap:10px;border-top:1px solid var(--b);align-items:flex-start}"
"input,textarea,button{background:var(--bg);color:var(--fg);border:1px solid var(--b);padding:10px;font:inherit;border-radius:4px}"
"textarea{flex:1;resize:none;height:60px}"
"button{cursor:pointer}"
"button:hover{border-color:var(--a)}"
"</style>"
"</head>"
"<body>"
"<header><b>quint</b><button onclick=\"dm()\">&#9680;</button></header>"
"<div id=\"m\"></div>"
"<div id=\"f\">"
"<input id=\"n\" placeholder=\"name\" style=\"width:100px\">"
"<textarea id=\"x\" placeholder=\"message\" onkeydown=\"if(event.key==='Enter'&&!event.shiftKey){event.preventDefault();s();}\"></textarea>"
"<input type=\"file\" id=\"i\" accept=\"image/*\" onchange=\"im(this)\" style=\"width:90px;padding:8px;font-size:0.8em\">"
"<button onclick=\"s()\">send</button>"
"</div>"
"<script>"
"if(localStorage.m==='1')document.body.classList.add('dark');"
"function dm(){document.body.classList.toggle('dark');localStorage.m=document.body.classList.contains('dark')?'1':'0';}"
"function e(s){return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;');}"
"function im(f){if(!f.files.length)return;const r=new FileReader();r.onload=()=>{x.value=r.result;s();};r.readAsDataURL(f.files[0]);f.value='';}"
"let last=0;"
"async function l(){"
"const r=await fetch('/m?since='+last);"
"const j=await r.json();"
"j.forEach(v=>{"
"last=v.i;"
"const d=document.createElement('div');"
"d.className='msg';"
"let h;"
"if(v.t.startsWith('```')&&v.t.endsWith('```')){"
"h='<pre><code>'+e(v.t.slice(3,-3))+'</code></pre>';"
"}else if(v.t.startsWith('data:image')){"
"h='<img src=\"'+v.t+'\">';"
"}else{"
"h=e(v.t);"
"}"
"d.innerHTML='<b>'+e(v.u)+'</b>: '+h;"
"m.appendChild(d);"
"m.scrollTop=m.scrollHeight;"
"});"
"}"
"async function s(){"
"if(!n.value||!x.value)return;"
"await fetch('/s',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'u='+encodeURIComponent(n.value)+'&t='+encodeURIComponent(x.value)});"
"x.value='';"
"l();"
"}"
"setInterval(l,1000);"
"l();"
"</script>"
"</body>"
"</html>";

void json_escape(const char *src, char *dst, size_t n) {
    size_t j = 0;
    for (size_t i = 0; src[i] && j < n - 1; i++) {
        unsigned char c = src[i];
        switch (c) {
            case '\"': if (j < n - 2) { dst[j++] = '\\'; dst[j++] = '\"'; } break;
            case '\\': if (j < n - 2) { dst[j++] = '\\'; dst[j++] = '\\'; } break;
            case '\n': if (j < n - 2) { dst[j++] = '\\'; dst[j++] = 'n'; } break;
            case '\r': if (j < n - 2) { dst[j++] = '\\'; dst[j++] = 'r'; } break;
            case '\t': if (j < n - 2) { dst[j++] = '\\'; dst[j++] = 't'; } break;
            case '\b': if (j < n - 2) { dst[j++] = '\\'; dst[j++] = 'b'; } break;
            case '\f': if (j < n - 2) { dst[j++] = '\\'; dst[j++] = 'f'; } break;
            default:
                if (c < 0x20) {
                    // drop other control chars
                } else {
                    dst[j++] = c;
                }
        }
    }
    dst[j] = '\0';
}

void print_ips(int port) {
    struct ifaddrs *ifaddr, *ifa;
    if (getifaddrs(&ifaddr) == -1) {
        printf("127.0.0.1:%d\n", port);
        return;
    }
    int found = 0;
    for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL) continue;
        if (ifa->ifa_addr->sa_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)ifa->ifa_addr;
            char ip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            printf("%s:%d\n", ip, port);
            found = 1;
        }
    }
    if (!found) printf("127.0.0.1:%d\n", port);
    freeifaddrs(ifaddr);
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

void handle_client(int c) {
    char buf[131072];
    int n = recv(c, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = 0;

    char method[8] = {0}, path[256] = {0};
    sscanf(buf, "%7s %255s", method, path);

    if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        int plen = (int)strlen(PAGE);
        char h[256];
        snprintf(h, sizeof(h), "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", plen);
        send(c, h, strlen(h), 0);
        send(c, PAGE, plen, 0);
    }
    else if (strcmp(method, "GET") == 0 && strncmp(path, "/m", 2) == 0) {
        int since = 0;
        sscanf(path, "/m?since=%d", &since);

        pthread_mutex_lock(&lock);
        strcpy(json_buf, "[");
        int first = 1;
        for (int i = 0; i < msg_cnt; i++) {
            if (msgs[i].id > since) {
                if (!first) strcat(json_buf, ",");
                first = 0;
                char item[MAX_MSG * 2 + 256];
                char eu[MAX_USER * 2 + 1];
                char et[MAX_MSG * 2 + 1];
                json_escape(msgs[i].user, eu, sizeof(eu));
                json_escape(msgs[i].text, et, sizeof(et));
                snprintf(item, sizeof(item), "{\"i\":%d,\"u\":\"%s\",\"t\":\"%s\"}", msgs[i].id, eu, et);
                if (strlen(json_buf) + strlen(item) < sizeof(json_buf) - 4) {
                    strcat(json_buf, item);
                }
            }
        }
        strcat(json_buf, "]");
        pthread_mutex_unlock(&lock);

        char h[256];
        int jlen = (int)strlen(json_buf);
        snprintf(h, sizeof(h), "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", jlen);
        send(c, h, strlen(h), 0);
        send(c, json_buf, jlen, 0);
    }
    else if (strcmp(method, "POST") == 0 && strcmp(path, "/s") == 0) {
        int cl = 0;
        char *clh = strstr(buf, "Content-Length: ");
        if (clh) sscanf(clh, "Content-Length: %d", &cl);

        char *body = strstr(buf, "\r\n\r\n");
        int header_len = body ? (int)(body - buf) + 4 : n;
        if (body) body += 4;

        if (body && cl > 0) {
            int body_len = n - header_len;
            while (body_len < cl && n < (int)sizeof(buf) - 1) {
                int r = recv(c, buf + n, sizeof(buf) - 1 - n, 0);
                if (r <= 0) break;
                n += r;
                buf[n] = '\0';
                body_len = n - header_len;
            }
        }

        if (body && cl > 0) {
            char *up = strstr(body, "u=");
            char *tp = NULL;
            if (up) {
                char *amp = strchr(up, '&');
                if (amp) tp = strstr(amp, "t=");
            }

            if (up && tp) {
                up += 2;
                tp += 2;

                char ue[128] = {0};
                char te[MAX_MSG] = {0};

                char *uep = strchr(up, '&');
                if (uep) {
                    int l = (int)(uep - up);
                    if (l >= (int)sizeof(ue)) l = sizeof(ue) - 1;
                    strncpy(ue, up, l);
                    ue[l] = '\0';
                } else {
                    strncpy(ue, up, sizeof(ue) - 1);
                    ue[sizeof(ue) - 1] = '\0';
                }

                char *tep = strchr(tp, '&');
                if (tep) {
                    int l = (int)(tep - tp);
                    if (l >= (int)sizeof(te)) l = sizeof(te) - 1;
                    strncpy(te, tp, l);
                    te[l] = '\0';
                } else {
                    strncpy(te, tp, sizeof(te) - 1);
                    te[sizeof(te) - 1] = '\0';
                }

                char u[MAX_USER] = {0};
                char t[MAX_MSG] = {0};
                url_decode(ue, u, sizeof(u));
                url_decode(te, t, sizeof(t));

                if (strlen(u) && strlen(t)) {
                    pthread_mutex_lock(&lock);
                    if (msg_cnt < MAX_HIST) {
                        msgs[msg_cnt].id = next_id++;
                        strncpy(msgs[msg_cnt].user, u, MAX_USER - 1);
                        msgs[msg_cnt].user[MAX_USER - 1] = '\0';
                        strncpy(msgs[msg_cnt].text, t, MAX_MSG - 1);
                        msgs[msg_cnt].text[MAX_MSG - 1] = '\0';
                        msg_cnt++;
                    } else {
                        for (int i = 1; i < MAX_HIST; i++) msgs[i-1] = msgs[i];
                        msgs[MAX_HIST-1].id = next_id++;
                        strncpy(msgs[MAX_HIST-1].user, u, MAX_USER - 1);
                        msgs[MAX_HIST-1].user[MAX_USER - 1] = '\0';
                        strncpy(msgs[MAX_HIST-1].text, t, MAX_MSG - 1);
                        msgs[MAX_HIST-1].text[MAX_MSG - 1] = '\0';
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

    print_ips(port);
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
