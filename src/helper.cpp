#include "core.h"
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <poll.h>
#include <set>
#include <string>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <systemd/sd-bus.h>
#include <systemd/sd-daemon.h>
#include <systemd/sd-login.h>
#include <unistd.h>

using namespace Windchime;
static int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
static bool bit(const unsigned long *bits, unsigned n) {
    return bits[n / (8 * sizeof(long))] & (1UL << (n % (8 * sizeof(long))));
}
struct Keyboard {
    int fd;
    std::set<unsigned> down;
};
class Reader {
    std::map<std::string, Keyboard> keyboards;

  public:
    LetterGate gate;
    ~Reader() {
        closeAll();
    }
    void closeAll() {
        for (auto &[p, k] : keyboards)
            close(k.fd);
        keyboards.clear();
        gate.reset();
    }
    size_t size() const {
        return keyboards.size();
    }
    void scan() {
        DIR *d = opendir("/dev/input");
        if (!d)
            return;
        std::set<std::string> present;
        while (auto *e = readdir(d)) {
            if (std::strncmp(e->d_name, "event", 5))
                continue;
            std::string path = "/dev/input/" + std::string(e->d_name);
            present.insert(path);
            if (keyboards.count(path))
                continue;
            int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0)
                continue;
            unsigned long keys[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
            input_id ident{};
            bool ok = ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys) >= 0 && bit(keys, KEY_A) &&
                      bit(keys, KEY_Z) && bit(keys, KEY_SPACE) && ioctl(fd, EVIOCGID, &ident) >= 0;
            // Exclude synthetic/virtual devices; listen to physical keyboards only.
            ok = ok && ident.bustype != BUS_VIRTUAL && ident.bustype != 0;
            if (!ok) {
                close(fd);
                continue;
            }
            unsigned long down[(KEY_MAX + 8 * sizeof(long)) / (8 * sizeof(long))] = {};
            Keyboard k{fd, {}};
            if (ioctl(fd, EVIOCGKEY(sizeof(down)), down) >= 0)
                for (unsigned i = 0; i <= KEY_MAX; i++)
                    if (bit(down, i))
                        k.down.insert(i);
            keyboards.emplace(path, std::move(k));
        }
        closedir(d);
        for (auto i = keyboards.begin(); i != keyboards.end();) {
            if (!present.count(i->first)) {
                close(i->second.fd);
                i = keyboards.erase(i);
                gate.reset();
            } else
                ++i;
        }
    }
    void appendPoll(std::vector<pollfd> &p) const {
        for (auto &[s, k] : keyboards)
            p.push_back({k.fd, POLLIN, 0});
    }
    template <typename Emit> void process(const std::vector<pollfd> &polls, Emit emit) {
        std::vector<std::string> remove;
        for (auto &[path, k] : keyboards) {
            auto p =
                std::find_if(polls.begin(), polls.end(), [&](auto &x) { return x.fd == k.fd; });
            if (p == polls.end())
                continue;
            if (p->revents & (POLLHUP | POLLERR | POLLNVAL)) {
                remove.push_back(path);
                continue;
            }
            if (!(p->revents & POLLIN))
                continue;
            input_event ev[64];
            ssize_t n;
            // Yield to the control socket and logind even during continuous input.
            int batches = 0;
            while (batches++ < 4 && (n = read(k.fd, ev, sizeof(ev))) > 0) {
                for (size_t i = 0; i < size_t(n) / sizeof(input_event); i++) {
                    const auto &v = ev[i];
                    if (v.type == EV_SYN && v.code == SYN_DROPPED) {
                        // Discard uncertain input and reopen with a fresh modifier snapshot.
                        remove.push_back(path);
                        gate.reset();
                        break;
                    }
                    if (v.type != EV_KEY)
                        continue;
                    if (v.value == 0) {
                        k.down.erase(v.code);
                        continue;
                    }
                    k.down.insert(v.code);
                    bool shortcut = false;
                    for (auto &[s, other] : keyboards)
                        for (unsigned m : {KEY_LEFTCTRL, KEY_RIGHTCTRL, KEY_LEFTALT, KEY_RIGHTALT,
                                           KEY_LEFTMETA, KEY_RIGHTMETA})
                            shortcut |= other.down.count(m) > 0;
                    int note = gate.press(v.code, v.value, shortcut);
                    if (note >= 0)
                        emit(note);
                }
                if (std::find(remove.begin(), remove.end(), path) != remove.end())
                    break;
            }
            if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
                remove.push_back(path);
        }
        for (auto &path : remove) {
            auto i = keyboards.find(path);
            if (i != keyboards.end()) {
                close(i->second.fd);
                keyboards.erase(i);
            }
        }
    }
};
static bool changed = true;
static int propertiesChanged(sd_bus_message *, void *, sd_bus_error *) {
    changed = true;
    return 0;
}
static bool sessionSafe(sd_bus *bus, uid_t uid) {
    if (!bus)
        return false;
    char **sessions = nullptr;
    int count = sd_uid_get_sessions(uid, 1, &sessions);
    bool safe = false;
    for (int i = 0; i < count; i++) {
        char *type = nullptr, *seat = nullptr;
        bool candidate = sd_session_get_type(sessions[i], &type) >= 0 && type &&
                         (!strcmp(type, "wayland") || !strcmp(type, "x11")) &&
                         sd_session_get_seat(sessions[i], &seat) >= 0 && seat &&
                         !strcmp(seat, "seat0") && sd_session_is_remote(sessions[i]) == 0;
        free(type);
        free(seat);
        if (!candidate)
            continue;
        sd_bus_message *reply = nullptr;
        const char *path = nullptr;
        if (sd_bus_call_method(bus, "org.freedesktop.login1", "/org/freedesktop/login1",
                               "org.freedesktop.login1.Manager", "GetSession", nullptr, &reply, "s",
                               sessions[i]) >= 0 &&
            sd_bus_message_read(reply, "o", &path) >= 0) {
            int locked = 1, active = 0;
            safe = sd_bus_get_property_trivial(bus, "org.freedesktop.login1", path,
                                               "org.freedesktop.login1.Session", "LockedHint",
                                               nullptr, 'b', &locked) >= 0 &&
                   sd_bus_get_property_trivial(bus, "org.freedesktop.login1", path,
                                               "org.freedesktop.login1.Session", "Active", nullptr,
                                               'b', &active) >= 0 &&
                   active && !locked;
        }
        sd_bus_message_unref(reply);
        if (safe)
            break;
    }
    if (sessions) {
        for (int i = 0; sessions[i]; i++)
            free(sessions[i]);
        free(sessions);
    }
    return safe;
}
static bool sendLine(int fd, const std::string &s) {
    return send(fd, s.data(), s.size(), MSG_NOSIGNAL | MSG_DONTWAIT) == ssize_t(s.size());
}
int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: windchime-input UID\n");
        return 2;
    }
    char *end = nullptr;
    unsigned long parsed = strtoul(argv[1], &end, 10);
    if (!end || *end || parsed == 0 || parsed > UINT32_MAX)
        return 2;
    uid_t uid = uid_t(parsed);
    if (sd_listen_fds(1) != 1 || sd_is_socket(SD_LISTEN_FDS_START, AF_UNIX, SOCK_STREAM, 1) != 1) {
        fprintf(stderr, "Requires its systemd socket.\n");
        return 2;
    }
    int server = SD_LISTEN_FDS_START;
    fcntl(server, F_SETFL, O_NONBLOCK);
    sd_bus *bus = nullptr;
    if (sd_bus_open_system(&bus) < 0)
        return 1;
    sd_bus_set_method_call_timeout(bus, 100000);
    if (sd_bus_match_signal(bus, nullptr, "org.freedesktop.login1", nullptr,
                            "org.freedesktop.DBus.Properties", "PropertiesChanged",
                            propertiesChanged, nullptr) < 0) {
        sd_bus_unref(bus);
        return 1;
    }
    Reader reader;
    int client = -1;
    bool requested = false, safe = false;
    int64_t heartbeat = 0, lastSafety = 0, lastScan = 0;
    std::string commands, lastStatus;
    for (;;) {
        int64_t now = nowMs();
        if (bus) {
            int r = 0;
            for (int i = 0; i < 32 && (r = sd_bus_process(bus, nullptr)) > 0; i++) {
            }
            if (r < 0) {
                reader.closeAll();
                sd_bus_unref(bus);
                return 1;
            }
        }
        if (changed || now - lastSafety >= 250) {
            safe = sessionSafe(bus, uid);
            lastSafety = now;
            changed = false;
        }
        bool enabled = client >= 0 && requested && safe && now - heartbeat < 3000;
        if (!enabled) {
            reader.closeAll();
            lastScan = 0;
        } else if (now - lastScan >= 1500) {
            reader.scan();
            lastScan = now;
        }
        std::string state =
            client < 0
                ? "disconnected"
                : (!requested
                       ? "paused"
                       : (!safe ? "locked"
                                : (now - heartbeat >= 3000
                                       ? "paused"
                                       : (reader.size() == 0 ? "no-keyboard" : "listening"))));
        std::string status = "S " + state + " " + std::to_string(reader.size()) + "\n";
        if (client >= 0 && status != lastStatus) {
            if (!sendLine(client, status)) {
                close(client);
                client = -1;
                requested = false;
                reader.closeAll();
            } else
                lastStatus = status;
        }
        std::vector<pollfd> p = {{server, POLLIN, 0}};
        if (client >= 0)
            p.push_back({client, POLLIN, 0});
        if (bus)
            p.push_back({sd_bus_get_fd(bus), POLLIN, 0});
        if (enabled)
            reader.appendPoll(p);
        int r = poll(p.data(), p.size(), 100);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return 1;
        }
        if (p[0].revents & POLLIN) {
            int fd = accept4(server, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd >= 0) {
                ucred creds{};
                socklen_t len = sizeof(creds);
                if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &creds, &len) < 0 || creds.uid != uid ||
                    client >= 0)
                    close(fd);
                else {
                    client = fd;
                    commands.clear();
                    requested = false;
                    heartbeat = nowMs();
                    lastStatus.clear();
                }
            }
        }
        if (client >= 0) {
            auto cp = std::find_if(p.begin(), p.end(), [&](auto &x) { return x.fd == client; });
            if (cp != p.end() && cp->revents & (POLLIN | POLLHUP | POLLERR)) {
                char buf[128];
                ssize_t n = recv(client, buf, sizeof(buf), MSG_DONTWAIT);
                if (n <= 0 && (n == 0 || errno != EAGAIN)) {
                    close(client);
                    client = -1;
                    requested = false;
                    reader.closeAll();
                    commands.clear();
                } else if (n > 0) {
                    commands.append(buf, n);
                    if (commands.size() > 512) {
                        close(client);
                        client = -1;
                        requested = false;
                        reader.closeAll();
                        commands.clear();
                    } else {
                        size_t pos;
                        while ((pos = commands.find('\n')) != std::string::npos) {
                            std::string line = commands.substr(0, pos);
                            commands.erase(0, pos + 1);
                            int density = 0;
                            bool on = false;
                            if (!parseConfig(line, density, on)) {
                                requested = false;
                                reader.closeAll();
                            } else {
                                reader.gate.configure(density);
                                requested = on;
                                heartbeat = nowMs();
                                if (!requested)
                                    reader.closeAll();
                            }
                        }
                    }
                }
            }
        }
        // Re-check lock notifications before handling a ready keyboard batch.
        if (bus) {
            int r = 0;
            for (int i = 0; i < 32 && (r = sd_bus_process(bus, nullptr)) > 0; i++) {
            }
            if (r < 0) {
                reader.closeAll();
                sd_bus_unref(bus);
                return 1;
            }
        }
        if (changed) {
            safe = sessionSafe(bus, uid);
            changed = false;
            lastSafety = nowMs();
        }
        if (client >= 0 && requested && safe && nowMs() - heartbeat < 3000) {
            bool failed = false;
            reader.process(p, [&](int note) {
                if (!sendLine(client, "N " + std::to_string(note) + "\n"))
                    failed = true;
            });
            if (failed) {
                close(client);
                client = -1;
                requested = false;
                reader.closeAll();
            }
        } else
            reader.closeAll();
    }
}
