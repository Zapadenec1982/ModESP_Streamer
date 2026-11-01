
#include <atomic>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include "dlna_browser.h"
#include "upnp_xml.h"
// dlna_browser.cpp's own separator for a message's arguments: the functions taken from
// it use the macro, which is defined at the top of that file and not in the parts copied here.
#define MSG_ARG "\x1f"
namespace dlna_client {
using QueueHandle_t=void*;using SemaphoreHandle_t=void*;
constexpr int portMAX_DELAY=0;
void xSemaphoreTake(void*,int){};void xSemaphoreGive(void*){};
struct sockaddr{};struct sockaddr_in{int sin_family,sin_port,sin_addr;};
struct timeval {int tv_sec,tv_usec;};
constexpr int AF_INET=2,SOCK_DGRAM=2,IPPROTO_UDP=17,SOL_SOCKET=1,SO_RCVTIMEO=2;
int socket(int,int,int){return 1;}int setsockopt(int,int,int,const void*,size_t){return 0;}
int htons(int n){return n;}int inet_pton(int,const char*,int*){return 1;}
int sendto(int,const char*,size_t,int,sockaddr*,size_t){return 1;}
void close(int){};int64_t esp_timer_get_time(){return 0;}
constexpr size_t XML_CAP = 65536, ART_CAP = 512 * 1024;
constexpr int SERVERS = 8, DEPTH = 16;
struct Server {
    char name[96]{}, location[257]{}, control[257]{}, type[96]{};
};
struct Entry {
    char id[257]{};
    dlna_core::Track track{};
    bool folder = false, supported = false;
};
struct Frame {
    char id[257]{}, name[96]{};
    uint32_t offset = 0;
};
struct State {
    View view{};
    Server servers[SERVERS]{};
    int server_count = 0, selected = -1;
    Entry entries[PAGE_SIZE]{};
    Frame path[DEPTH]{};
    int depth = 0;
    uint32_t total = 0, returned = 0;
    dlna_core::Track selected_track{};
    bool track_ready = false, queue_next = false;
    char art_uri[257]{};
    uint32_t art_generation = 0;
    std::atomic<uint32_t> art_serial{0}, serial{0};
    std::atomic<bool> run{true};
    char *http = nullptr;
    char *xml = nullptr;
    QueueHandle_t commands = nullptr;
    SemaphoreHandle_t guard = nullptr;
};
State *s = nullptr;
struct Request {
    Command command;
    uint8_t row;
    uint16_t revision;
    uint32_t serial;
};
struct Lock {
    Lock() {
        xSemaphoreTake(s->guard, portMAX_DELAY);
    }
    ~Lock() {
        xSemaphoreGive(s->guard);
    }
};

int recv(int,char*,size_t,int){++s->serial;return -1;} // a concurrent Cancel cancels discovery
bool describe(Server&,uint32_t){return false;}
int browse_selected=999;
bool browse(uint32_t){browse_selected=s->selected;printf("browse called with selected=%d (servers[%d])\n",s->selected,s->selected);return false;}
void status(const char *message, bool busy = false) {
    Lock lock;
    snprintf(s->view.status, sizeof(s->view.status), "%s", message);
    s->view.busy = busy;
}
void publish(bool servers) {
    Lock lock;
    auto &v = s->view;
    ++v.revision;
    if (!v.revision)
        ++v.revision;
    v.servers = servers;
    v.busy = false;
    v.can_back = !servers;
    v.has_prev = !servers && s->path[s->depth].offset > 0;
    v.has_next = !servers && s->returned > 0 && s->path[s->depth].offset + s->returned < s->total;
    v.count = servers ? s->server_count : (s->returned > PAGE_SIZE ? PAGE_SIZE : s->returned);
    snprintf(v.title, sizeof(v.title), "%s", servers ? "DLNA servers" : s->path[s->depth].name);
    for (int i = 0; i < PAGE_SIZE; ++i) {
        v.rows[i] = Row{};
        if (i >= v.count)
            continue;
        auto &row = v.rows[i];
        if (servers) {
            snprintf(row.title, sizeof(row.title), "%s", s->servers[i].name);
            row.folder = true;
        } else {
            auto &e = s->entries[i];
            snprintf(row.title, sizeof(row.title), "%s", e.track.title);
            snprintf(row.subtitle, sizeof(row.subtitle), "%s", e.track.artist);
            row.folder = e.folder;
            row.playable = e.supported;
        }
    }
    snprintf(v.status, sizeof(v.status), servers ? "%d server(s)" : "%d item(s) on this page",
             v.count);
}
void discover(uint32_t serial) {
    {
        Lock lock;
        s->view.count = 0;
        ++s->view.revision;
    }
    status("Searching...", true);
    s->server_count = 0;
    s->selected = -1;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        status("No discovery socket available");
        return;
    }
    timeval wait{0, 200000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof(wait));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(1900);
    inet_pton(AF_INET, "239.255.255.250", &address.sin_addr);
    const char query[] =
        "M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: \"ssdp:discover\"\r\nMX: "
        "2\r\nST: urn:schemas-upnp-org:device:MediaServer:1\r\n\r\n";
    sendto(fd, query, sizeof(query) - 1, 0, (sockaddr *)&address, sizeof(address));
    int64_t end = esp_timer_get_time() + 2500000;
    while (s->run && s->serial.load() == serial && esp_timer_get_time() < end &&
           s->server_count < SERVERS) {
        char packet[2048];
        int n = recv(fd, packet, sizeof(packet) - 1, 0);
        if (n <= 0)
            continue;
        packet[n] = 0;
        const char *p = packet;
        char location[257]{};
        while (*p) {
            const char *nl = strstr(p, "\r\n");
            if (!nl)
                break;
            if (!strncasecmp(p, "location:", 9)) {
                const char *first = p + 9;
                while (first < nl && *first == ' ')
                    ++first;
                size_t size = nl - first;
                if (size < sizeof(location)) {
                    memcpy(location, first, size);
                    location[size] = 0;
                }
            }
            p = nl + 2;
        }
        if (!dlna_core::http_url(location))
            continue;
        bool duplicate = false;
        for (int i = 0; i < s->server_count; ++i)
            duplicate |= !strcmp(location, s->servers[i].location);
        if (duplicate)
            continue;
        auto &server = s->servers[s->server_count++];
        server = Server{};
        snprintf(server.location, sizeof(server.location), "%s", location);
    }
    close(fd);
    for (int i = 0; i < s->server_count;) {
        if (s->serial.load() != serial)
            return;
        if (describe(s->servers[i], serial))
            ++i;
        else {
            for (int j = i + 1; j < s->server_count; ++j)
                s->servers[j - 1] = s->servers[j];
            --s->server_count;
        }
    }
    if (s->serial.load() == serial)
        publish(true);
}
void process(Request r) {
    if (r.command == Command::Discover) {
        discover(r.serial);
        return;
    }
    if (r.command == Command::Cancel) {
        status("Cancelled");
        return;
    }
    bool root;
    {
        Lock lock;
        if (r.revision != s->view.revision) {
            s->view.busy = false;
            return;
        }
        root = s->view.servers;
    }
    if (r.command == Command::Back) {
        if (root)
            return;
        if (!s->depth) {
            publish(true);
            return;
        }
        --s->depth;
        browse(r.serial);
        return;
    }
    if (r.command == Command::NextPage || r.command == Command::PrevPage) {
        if (root)
            return;
        auto &offset = s->path[s->depth].offset;
        if (r.command == Command::NextPage) {
            if (!s->returned || offset + s->returned >= s->total)
                return;
            offset += s->returned;
        } else
            offset = offset > PAGE_SIZE ? offset - PAGE_SIZE : 0;
        browse(r.serial);
        return;
    }
    if (r.row >= (root ? s->server_count : s->returned))
        return;
    if (root) {
        s->selected = r.row;
        s->depth = 0;
        s->path[0] = Frame{};
        snprintf(s->path[0].id, sizeof(s->path[0].id), "0");
        snprintf(s->path[0].name, sizeof(s->path[0].name), "%s", s->servers[r.row].name);
        browse(r.serial);
        return;
    }
    auto &entry = s->entries[r.row];
    if (entry.folder) {
        if (!entry.id[0] || s->depth + 1 == DEPTH) {
            status("Folder depth or ID limit");
            return;
        }
        ++s->depth;
        auto &f = s->path[s->depth];
        f = Frame{};
        memmove(f.id, entry.id, sizeof(f.id));
        memmove(f.name, entry.track.title, sizeof(f.name));
        browse(r.serial);
        return;
    }
    if (!entry.supported) {
        status("No supported audio resource");
        return;
    }
    {
        Lock lock;
        s->selected_track = entry.track;
        s->queue_next = r.command == Command::Queue;
        s->track_ready = true;
        s->view.busy = false;
    }
    char message[96];
    snprintf(message, sizeof(message), "%s: %.70s",
             r.command == Command::Queue ? "Next" : "Playing", entry.track.title);
    status(message);
}

} // namespace
int main(){
 using namespace dlna_client;
 State state;s=&state;s->view.servers=false;s->view.can_back=true;s->view.revision=2;
 s->depth=1;s->selected=0;s->server_count=1;s->serial.store(10);
 process({Command::Discover,0,2,10});
 process({Command::Cancel,0,s->view.revision,11});
 process({Command::Back,0,s->view.revision,12});
 if(!s->view.servers || s->view.can_back || s->depth!=0 || s->selected!=-1 ||
    s->server_count!=0 || browse_selected!=999) return 1;
 if(browse_target_valid()) return 2;
 s->selected=0;s->server_count=1;
 if(!browse_target_valid()) return 3;
 puts("DLNA refresh-cancel-back passed");
 return 0;
}
