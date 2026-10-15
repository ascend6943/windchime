#include "core.h"
#include <QCloseEvent>
#include <QElapsedTimer>
#include <QPainter>
#include <QtDBus>
#include <QtNetwork>
#include <QtWidgets>
#include <cstdio>
#include <pulse/pulseaudio.h>
#include <unistd.h>

using namespace Windchime;
struct Shortcut {
    QString id;
    QVariantMap options;
};
Q_DECLARE_METATYPE(Shortcut)
Q_DECLARE_METATYPE(QList<Shortcut>)
QDBusArgument &operator<<(QDBusArgument &a, const Shortcut &s) {
    a.beginStructure();
    a << s.id << s.options;
    a.endStructure();
    return a;
}
const QDBusArgument &operator>>(const QDBusArgument &a, Shortcut &s) {
    a.beginStructure();
    a >> s.id >> s.options;
    a.endStructure();
    return a;
}

static QIcon chimeIcon(bool on = true) {
    QPixmap pix(128, 128);
    pix.fill(Qt::transparent);
    QPainter p(&pix);
    p.setRenderHint(QPainter::Antialiasing);
    QColor color = on ? QColor("#8de0ce") : QColor("#87939d");
    p.setPen(QPen(color, 5, Qt::SolidLine, Qt::RoundCap));
    p.drawArc(QRectF(22, 14, 84, 30), 0, 180 * 16);
    p.drawLine(22, 29, 106, 29);
    for (int i = 0; i < 4; i++) {
        int x = 34 + i * 20, top = 45 + i % 2 * 9, bottom = 95 - i % 2 * 7;
        p.drawLine(x, 30, x, top);
        p.setBrush(color);
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(x - 4, top, 8, bottom - top), 4, 4);
        p.setPen(QPen(color, 5, Qt::SolidLine, Qt::RoundCap));
    }
    p.drawLine(64, 30, 64, 111);
    p.setBrush(QColor("#e9c099"));
    p.setPen(Qt::NoPen);
    p.drawEllipse(QPointF(64, 109), 8, 5);
    return QIcon(pix);
}
class Window : public QWidget {
  public:
    bool reallyClose = false;

  protected:
    void closeEvent(QCloseEvent *e) override {
        if (reallyClose)
            e->accept();
        else {
            e->ignore();
            hide();
        }
    }
};
class Audio : public QObject {
    Mixer mixer;
    pa_mainloop *loop = nullptr;
    pa_context *context = nullptr;
    pa_stream *stream = nullptr;
    QTimer pump;
    std::array<float, 480 * 2> chunk{};
    int tone = 0;
    double decay = 3;
    float volume = .35;
    QElapsedTimer connecting;
    bool draining = false;

  public:
    QString error;
    explicit Audio(QObject *parent = nullptr) : QObject(parent) {
        pump.setInterval(8);
        pump.setTimerType(Qt::PreciseTimer);
        connect(&pump, &QTimer::timeout, this, [this] { fill(); });
    }
    void configure(int t, double d, float v) {
        mixer.volume(v);
        volume = v;
        if (t != tone || d != decay) {
            tone = t;
            decay = d;
            stop();
            mixer.rebuild(t, d);
        }
    }
    bool start() {
        if (loop)
            return true;
        loop = pa_mainloop_new();
        if (!loop) {
            error = "Couldn't start audio.";
            return false;
        }
        auto *props = pa_proplist_new();
        pa_proplist_sets(props, PA_PROP_APPLICATION_NAME, "Windchime");
        pa_proplist_sets(props, PA_PROP_APPLICATION_ICON_NAME, "windchime");
        pa_proplist_sets(props, PA_PROP_MEDIA_ROLE, "music");
        context = pa_context_new_with_proplist(pa_mainloop_get_api(loop), "Windchime", props);
        pa_proplist_free(props);
        if (!context || pa_context_connect(context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
            error = "Audio output is unavailable. Check Linux sound settings.";
            stop();
            return false;
        }
        connecting.start();
        draining = false;
        error.clear();
        return true;
    }
    bool strike(int note, double strength = 0.8) {
        if (!start())
            return false;
        draining = false;
        mixer.strike(note, strength);
        pump.start();
        fill();
        return true;
    }
    void fill() {
        if (!loop || !context)
            return;
        // Service native audio asynchronously; never block the UI on sound writes.
        int result = 0;
        for (int i = 0; i < 8; i++) {
            int r = pa_mainloop_iterate(loop, 0, &result);
            if (r < 0) {
                error = "Audio connection interrupted.";
                stop();
                return;
            }
            if (r == 0)
                break;
        }
        auto state = pa_context_get_state(context);
        if (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED) {
            error = "Audio output disconnected. Check Linux sound settings.";
            stop();
            return;
        }
        if (state != PA_CONTEXT_READY) {
            if (connecting.elapsed() > 2500) {
                error = "Audio connection timed out.";
                stop();
            }
            return;
        }
        if (!stream) {
            pa_sample_spec spec{PA_SAMPLE_FLOAT32NE, Rate, 2};
            stream = pa_stream_new(context, "Windchimes", &spec, nullptr);
            pa_buffer_attr attr{UINT32_MAX, 480 * 2 * sizeof(float) * 4, 0, 480 * 2 * sizeof(float),
                                UINT32_MAX};
            if (!stream ||
                pa_stream_connect_playback(
                    stream, nullptr, &attr,
                    pa_stream_flags_t(PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE),
                    nullptr, nullptr) < 0) {
                error = "Couldn't open the audio output.";
                stop();
                return;
            }
        }
        auto streamState = pa_stream_get_state(stream);
        if (streamState == PA_STREAM_FAILED || streamState == PA_STREAM_TERMINATED) {
            error = "Audio output disconnected.";
            stop();
            return;
        }
        if (streamState != PA_STREAM_READY) {
            if (connecting.elapsed() > 2500) {
                error = "Audio output connection timed out.";
                stop();
            }
            return;
        }
        int blocks = 0;
        size_t available = pa_stream_writable_size(stream);
        if (available == size_t(-1)) {
            error = "Audio output unavailable.";
            stop();
            return;
        }
        while (available >= chunk.size() * sizeof(float) && blocks++ < 4) {
            mixer.render(chunk.data(), 480);
            if (pa_stream_write(stream, chunk.data(), chunk.size() * sizeof(float), nullptr, 0,
                                PA_SEEK_RELATIVE) < 0) {
                error = "Couldn't write audio.";
                stop();
                return;
            }
            available -= chunk.size() * sizeof(float);
        }
        if (!mixer.active() && !draining) {
            draining = true;
            // Let the final samples drain, then release the stream entirely at idle.
            QTimer::singleShot(100, this, [this] {
                if (!mixer.active() && draining)
                    stop();
            });
        }
    }
    void stop() {
        pump.stop();
        mixer.clear();
        draining = false;
        if (stream) {
            pa_stream_disconnect(stream);
            pa_stream_unref(stream);
            stream = nullptr;
        }
        if (context) {
            pa_context_disconnect(context);
            pa_context_unref(context);
            context = nullptr;
        }
        if (loop) {
            pa_mainloop_free(loop);
            loop = nullptr;
        }
    }
};

class App : public QObject {
    Q_OBJECT
    QSettings settings{"Windchime", "Windchime"};
    Window window;
    QSystemTrayIcon tray;
    QMenu menu;
    QAction *toggleAction = nullptr;
    QLabel *statusLabel = nullptr, *densityValue = nullptr, *paceValue = nullptr,
           *decayValue = nullptr, *volumeValue = nullptr, *shortcutLabel = nullptr,
           *activityLabel = nullptr;
    QPushButton *toggleButton = nullptr, *installButton = nullptr;
    QSlider *densitySlider = nullptr, *paceSlider = nullptr, *decaySlider = nullptr,
            *volumeSlider = nullptr;
    QComboBox *toneBox = nullptr;
    QCheckBox *autostartBox = nullptr;
    QLocalSocket helper;
    QLocalServer control;
    Audio audio{this};
    QTimer heartbeat, reconnect, scheduler;
    QElapsedTimer clock;
    NoteQueue queue;
    QByteArray incoming;
    QString helperState = "missing", portalSession, shortcutDescription = "Not assigned";
    bool enabled = true, screenLocked = true, lockKnown = false, requestingShortcut = false,
         previewing = false;
    const bool diagnosticMode =
        qApp->arguments().contains("--ui-check") || qApp->arguments().contains("--render");
    int keyboardCount = 0, previewGeneration = 0;
    quint64 lockRevision = 0;
    quint64 sounded = 0;
    QString controlPath;
    int density() const {
        return densitySlider ? densitySlider->value() : 5;
    }
    bool listening() const {
        return enabled && lockKnown && !screenLocked && helperState == "listening";
    }
    static QString runtimePath() {
        return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) +
               "/windchime-control.sock";
    }
    QLabel *label(const QString &text, const QString &name = "") {
        auto *l = new QLabel(text);
        if (!name.isEmpty())
            l->setObjectName(name);
        return l;
    }
    QSlider *slider(int lo, int hi, int value) {
        auto *s = new QSlider(Qt::Horizontal);
        s->setRange(lo, hi);
        s->setValue(std::clamp(value, lo, hi));
        s->setMinimumWidth(200);
        return s;
    }
    QWidget *card(const QString &title, const QString &description, QWidget *control,
                  QLabel *value) {
        auto *w = new QFrame;
        w->setObjectName("card");
        auto *v = new QVBoxLayout(w);
        v->setContentsMargins(20, 15, 20, 15);
        v->setSpacing(8);
        auto *top = new QHBoxLayout;
        top->addWidget(label(title, "cardTitle"));
        top->addStretch();
        top->addWidget(value);
        v->addLayout(top);
        auto *desc = label(description, "hint");
        desc->setWordWrap(true);
        v->addWidget(desc);
        v->addWidget(control);
        return w;
    }
    void buildWindow() {
        window.setWindowTitle("Windchime");
        window.setWindowIcon(chimeIcon());
        window.resize(620, 780);
        window.setMinimumWidth(520);
        window.setStyleSheet(R"(
          QWidget {background:#17252d;color:#e8eff0;font-family:"Noto Sans";font-size:13px;}
          QLabel {background:transparent;}
          QLabel#title {font-size:30px;font-weight:600;letter-spacing:1px;color:#9de2d2;}
          QLabel#subtitle {color:#bbcacc;font-size:14px;}
          QLabel#hint {color:#a4b9bc;font-size:12px;}
          QLabel#cardTitle {font-size:14px;font-weight:600;}
          QLabel#value {color:#9de2d2;font-size:14px;font-weight:600;}
          QFrame#card {background:#20343e;border:1px solid #314a54;border-radius:12px;}
          QFrame#card QWidget {background:transparent;}
          QPushButton {background:#304853;border:1px solid #46616a;border-radius:8px;padding:10px 17px;font-weight:600;}
          QPushButton:hover {background:#3c5963;border-color:#83cbbd;}
          QPushButton#primary {background:#9de2d2;color:#17252d;border:0;}
          QPushButton#primary:hover {background:#b3eedf;}
          QPushButton:disabled {color:#789093;}
          QSlider::groove:horizontal {height:5px;background:#405761;border-radius:2px;}
          QSlider::sub-page:horizontal {background:#9de2d2;border-radius:2px;}
          QSlider::handle:horizontal {width:16px;margin:-6px 0;background:#a9e8d9;border-radius:8px;}
          QComboBox {background:#304853;border:1px solid #46616a;border-radius:7px;padding:8px;}
          QComboBox QAbstractItemView {background:#20343e;selection-background-color:#3c5963;}
          QCheckBox {spacing:9px;color:#b7c9cb;}
          QScrollArea {border:0;}
          QScrollBar:vertical {background:transparent;width:7px;margin:0;}
          QScrollBar::handle:vertical {background:#405761;border-radius:3px;min-height:24px;}
          QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {height:0;}
          QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {background:transparent;}
          QToolTip {background:#20343e;color:#e8eff0;border:1px solid #46616a;}
        )");
        auto *outer = new QVBoxLayout(&window);
        outer->setContentsMargins(0, 0, 0, 0);
        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        auto *body = new QWidget;
        auto *v = new QVBoxLayout(body);
        v->setContentsMargins(28, 24, 28, 24);
        v->setSpacing(13);
        auto *head = new QHBoxLayout;
        auto *icon = new QLabel;
        icon->setPixmap(chimeIcon().pixmap(62, 62));
        head->addWidget(icon);
        auto *titles = new QVBoxLayout;
        titles->addWidget(label("Windchime", "title"));
        titles->addWidget(label("A little music in the things you write.", "subtitle"));
        head->addLayout(titles);
        head->addStretch();
        v->addLayout(head);
        auto *row = new QHBoxLayout;
        statusLabel = label("Connecting…", "value");
        statusLabel->setWordWrap(true);
        row->addWidget(statusLabel, 1);
        toggleButton = new QPushButton("Pause");
        toggleButton->setObjectName("primary");
        row->addWidget(toggleButton);
        v->addLayout(row);
        connect(toggleButton, &QPushButton::clicked, this, [this] { setEnabled(!enabled); });
        densitySlider = slider(1, 10, settings.value("letters", 5).toInt());
        densityValue = label("", "value");
        v->addWidget(card("Letters per chime",
                          "1 plays every letter. Higher values leave more room to breathe.",
                          densitySlider, densityValue));
        paceSlider = slider(40, 800, settings.value("spacing", 150).toInt());
        paceValue = label("", "value");
        v->addWidget(card("Playback spacing",
                          "Minimum time between notes. Quick typing makes a short, flowing phrase.",
                          paceSlider, paceValue));
        auto *soundCard = new QFrame;
        soundCard->setObjectName("card");
        auto *soundV = new QVBoxLayout(soundCard);
        soundV->setContentsMargins(20, 15, 20, 15);
        soundV->setSpacing(12);
        auto *voiceRow = new QHBoxLayout;
        voiceRow->addWidget(label("Chime character", "cardTitle"));
        voiceRow->addStretch();
        toneBox = new QComboBox;
        toneBox->addItems({"Warm · mellow metal", "Airy · light shimmer", "Bright · clear bells"});
        toneBox->setCurrentIndex(std::clamp(settings.value("tone", 0).toInt(), 0, 2));
        voiceRow->addWidget(toneBox);
        soundV->addLayout(voiceRow);
        auto *volumeRow = new QHBoxLayout;
        volumeRow->addWidget(label("Volume"));
        volumeSlider = slider(0, 100, settings.value("volume", 35).toInt());
        volumeRow->addWidget(volumeSlider, 1);
        volumeValue = label("", "value");
        volumeValue->setMinimumWidth(48);
        volumeRow->addWidget(volumeValue);
        soundV->addLayout(volumeRow);
        auto *decayRow = new QHBoxLayout;
        decayRow->addWidget(label("Ring duration"));
        decaySlider = slider(10, 60, settings.value("decay", 30).toInt());
        decayRow->addWidget(decaySlider, 1);
        decayValue = label("", "value");
        decayValue->setMinimumWidth(48);
        decayRow->addWidget(decayValue);
        soundV->addLayout(decayRow);
        v->addWidget(soundCard);
        auto *previewRow = new QHBoxLayout;
        auto *previewButton = new QPushButton("♫  Preview windchimes");
        previewRow->addWidget(previewButton);
        activityLabel = label("Ready to make a little music.", "hint");
        previewRow->addWidget(activityLabel, 1);
        v->addLayout(previewRow);
        connect(previewButton, &QPushButton::clicked, this, [this] { preview(); });
        auto *bottom = new QHBoxLayout;
        autostartBox = new QCheckBox("Start when I sign in");
        autostartBox->setChecked(settings.value("autostart", true).toBool());
        bottom->addWidget(autostartBox);
        bottom->addStretch();
        auto *shortcutButton = new QPushButton("Set pause shortcut…");
        bottom->addWidget(shortcutButton);
        v->addLayout(bottom);
        shortcutLabel = label("Pause shortcut: " + shortcutDescription, "hint");
        v->addWidget(shortcutLabel);
        connect(shortcutButton, &QPushButton::clicked, this, [this] { createShortcut(); });
        installButton = new QPushButton("Enable keyboard access…");
        v->addWidget(installButton);
        connect(installButton, &QPushButton::clicked, this, [this] { installHelper(); });
        auto *privacy = label("Sound only. No typing history is saved. Pause for sensitive "
                              "typing.\nClosing this window keeps Windchime in your tray.",
                              "hint");
        privacy->setWordWrap(true);
        v->addWidget(privacy);
        v->addStretch();
        scroll->setWidget(body);
        outer->addWidget(scroll);
        for (auto *s : {densitySlider, paceSlider, volumeSlider, decaySlider})
            connect(s, &QSlider::valueChanged, this, [this] { saveControls(); });
        connect(toneBox, &QComboBox::currentIndexChanged, this, [this] { saveControls(); });
        connect(autostartBox, &QCheckBox::toggled, this, [this](bool on) {
            setAutostart(on);
            settings.setValue("autostart", on);
        });
        saveControls();
    }
    void saveControls() {
        int d = density();
        densityValue->setText(d == 1 ? "Every letter" : QString("Every %1 letters").arg(d));
        paceValue->setText(QString("%1 ms").arg(paceSlider->value()));
        volumeValue->setText(QString("%1%").arg(volumeSlider->value()));
        decayValue->setText(QString::number(decaySlider->value() / 10.0, 'f', 1) + " s");
        queue.spacing = paceSlider->value();
        audio.configure(toneBox->currentIndex(), decaySlider->value() / 10.0,
                        volumeSlider->value() / 100.f);
        settings.setValue("letters", d);
        settings.setValue("spacing", paceSlider->value());
        settings.setValue("volume", volumeSlider->value());
        settings.setValue("decay", decaySlider->value());
        settings.setValue("tone", toneBox->currentIndex());
        sendConfig();
    }
    void setAutostart(bool on) {
        auto dir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart";
        QDir().mkpath(dir);
        auto path = dir + "/windchime.desktop";
        if (!on) {
            QFile::remove(path);
            return;
        }
        QSaveFile f(path);
        if (f.open(QIODevice::WriteOnly)) {
            QByteArray s =
                "[Desktop Entry]\nType=Application\nName=Windchime\nComment=Gentle windchimes as "
                "you type\nExec=\"" +
                QCoreApplication::applicationFilePath().toUtf8() +
                "\" --tray\nIcon=windchime\nTerminal=false\nX-KDE-autostart-after=panel\n";
            f.write(s);
            f.commit();
        }
    }
    void show() {
        window.show();
        window.raise();
        window.activateWindow();
    }
    void sendConfig() {
        if (helper.state() != QLocalSocket::ConnectedState)
            return;
        bool read = enabled && lockKnown && !screenLocked;
        helper.write(QString("C %1 %2\n").arg(density()).arg(read ? 1 : 0).toUtf8());
        helper.flush();
    }
    void connectHelper() {
        if (diagnosticMode)
            return;
        if (helper.state() == QLocalSocket::ConnectedState ||
            helper.state() == QLocalSocket::ConnectingState)
            return;
        helper.abort();
        helper.connectToServer("/run/windchime/input.sock");
    }
    void refreshLock() {
        if (diagnosticMode)
            return;
        auto revision = lockRevision;
        auto m = QDBusMessage::createMethodCall("org.freedesktop.ScreenSaver", "/ScreenSaver",
                                                "org.freedesktop.ScreenSaver", "GetActive");
        auto *w =
            new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(m, 1000), this);
        connect(w, &QDBusPendingCallWatcher::finished, this, [this, w, revision] {
            QDBusPendingReply<bool> r = *w;
            if (revision == lockRevision) {
                lockKnown = !r.isError();
                applyLock(lockKnown ? r.value() : true);
            }
            w->deleteLater();
        });
    }
    void applyLock(bool locked) {
        screenLocked = locked;
        if (locked) {
            queue.clear();
            scheduler.stop();
            previewGeneration++;
            previewing = false;
            audio.stop();
        }
        sendConfig();
        updateStatus();
    }
    void updateStatus() {
        QString message;
        if (!enabled)
            message = "Paused · take a breath";
        else if (helperState == "missing")
            message = "Preview ready · keyboard access needed";
        else if (!lockKnown)
            message = "Waiting for desktop session";
        else if (screenLocked || helperState == "locked")
            message = "Paused while your session is locked";
        else if (helperState == "no-keyboard")
            message = "Waiting for a physical keyboard";
        else if (helperState == "listening")
            message = "Listening · gentle chimes as you type";
        else
            message = "Connecting to keyboard access…";
        statusLabel->setText(message);
        toggleButton->setText(enabled ? "Pause" : "Resume");
        toggleAction->setText(enabled ? "Pause listening" : "Resume listening");
        tray.setIcon(chimeIcon(listening()));
        tray.setToolTip("Windchime — " + message);
        installButton->setVisible(!QFile::exists("/opt/windchime/windchime-input") &&
                                  helperState == "missing");
        if (!audio.error.isEmpty())
            activityLabel->setText(audio.error);
    }
    void setEnabled(bool on) {
        enabled = on;
        settings.setValue("enabled", on);
        queue.clear();
        previewGeneration++;
        previewing = false;
        audio.stop();
        sendConfig();
        updateStatus();
    }
    void receive() {
        incoming += helper.readAll();
        if (incoming.size() > 4096) {
            helper.abort();
            incoming.clear();
            return;
        }
        int pos;
        while ((pos = incoming.indexOf('\n')) >= 0) {
            auto line = incoming.left(pos);
            incoming.remove(0, pos + 1);
            auto bits = line.split(' ');
            if (bits.size() == 3 && bits[0] == "S") {
                QString state = QString::fromLatin1(bits[1]);
                if (QStringList{"listening", "paused", "locked", "no-keyboard"}.contains(state)) {
                    helperState = state;
                    keyboardCount = bits[2].toInt();
                    if (state != "listening") {
                        queue.clear();
                        if (!previewing)
                            audio.stop();
                    }
                    updateStatus();
                }
            } else if (bits.size() == 2 && bits[0] == "N" && listening()) {
                bool ok = false;
                int note = bits[1].toInt(&ok);
                if (ok && note >= 0 && note < NoteCount) {
                    queue.push(note, .78, clock.elapsed());
                    scheduler.start();
                }
            }
        }
    }
    void preview() {
        queue.clear();
        audio.stop();
        previewing = true;
        int generation = ++previewGeneration;
        constexpr int melody[6] = {0, 3, 1, 4, 6, 2};
        for (int i = 0; i < 6; i++)
            QTimer::singleShot(i * 380, this, [this, generation, n = melody[i]] {
                if (generation == previewGeneration) {
                    if (!audio.strike(n, .85))
                        activityLabel->setText(audio.error);
                    else
                        activityLabel->setText("A quiet breeze through the chimes…");
                }
            });
        QTimer::singleShot(2100, this, [this, generation] {
            if (generation == previewGeneration) {
                previewing = false;
                activityLabel->setText("Ready to make a little music.");
            }
        });
    }
    void installHelper() {
        QString installer = QCoreApplication::applicationDirPath() + "/install-system.sh";
        if (!QFile::exists(installer))
            installer = QCoreApplication::applicationDirPath() + "/../install-system.sh";
        if (!QFile::exists(installer)) {
            QMessageBox::information(
                &window, "Keyboard access",
                "Run the installer included with Windchime to enable keyboard access.");
            return;
        }
        installButton->setEnabled(false);
        installButton->setText("Waiting for administrator approval…");
        auto *p = new QProcess(this);
        p->setProgram("/usr/bin/pkexec");
        p->setArguments({installer, QString::number(getuid()), qEnvironmentVariable("USER")});
        connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus) {
            installButton->setEnabled(true);
            installButton->setText("Enable keyboard access…");
            if (code != 0)
                QMessageBox::information(
                    &window, "Keyboard access",
                    "Administrator authentication wasn't completed. Sound preview still works. You "
                    "can try again whenever you're ready.");
            else
                connectHelper();
            p->deleteLater();
        });
        p->start();
    }
    void callPortal(const QString &method, const QVariantList &args) {
        auto m = QDBusMessage::createMethodCall("org.freedesktop.portal.Desktop",
                                                "/org/freedesktop/portal/desktop",
                                                "org.freedesktop.portal.GlobalShortcuts", method);
        m.setArguments(args);
        auto *w = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(m), this);
        connect(w, &QDBusPendingCallWatcher::finished, this, [this, w] {
            QDBusPendingReply<QDBusObjectPath> r = *w;
            if (r.isError()) {
                requestingShortcut = false;
                shortcutLabel->setText("Shortcut setup unavailable · pause from the tray");
            }
            w->deleteLater();
        });
    }
    void createShortcut() {
        if (requestingShortcut)
            return;
        if (!portalSession.isEmpty()) {
            QMessageBox::information(&window, "Pause shortcut",
                                     "Your pause shortcut is " + shortcutDescription +
                                         ". You can change it in KDE System Settings → Keyboard → "
                                         "Shortcuts → Windchime.");
            return;
        }
        requestingShortcut = true;
        QString token =
            "windchime_create_" + QString::number(QRandomGenerator::global()->generate());
        callPortal(
            "CreateSession",
            {QVariantMap{{"handle_token", token},
                         {"session_handle_token",
                          "windchime" + QString::number(QRandomGenerator::global()->generate())}}});
    }

  public:
    explicit App(bool trayOnly) {
        clock.start();
        enabled = settings.value("enabled", true).toBool();
        tray.setIcon(chimeIcon());
        tray.setContextMenu(&menu);
        menu.addAction("Open Windchime", this, [this] { show(); });
        toggleAction = menu.addAction("Pause listening", this, [this] { setEnabled(!enabled); });
        menu.addAction("Preview windchimes", this, [this] { preview(); });
        menu.addSeparator();
        menu.addAction("Quit", this, [this] {
            helper.abort();
            queue.clear();
            audio.stop();
            qApp->quit();
        });
        buildWindow();
        tray.show();
        connect(&tray, &QSystemTrayIcon::activated, this,
                [this](QSystemTrayIcon::ActivationReason r) {
                    if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick)
                        show();
                    if (r == QSystemTrayIcon::MiddleClick)
                        setEnabled(!enabled);
                });
        connect(&helper, &QLocalSocket::connected, this, [this] {
            incoming.clear();
            helperState = "connecting";
            sendConfig();
            updateStatus();
        });
        connect(&helper, &QLocalSocket::readyRead, this, &App::receive);
        connect(&helper, &QLocalSocket::disconnected, this, [this] {
            helperState = "missing";
            queue.clear();
            if (!previewing)
                audio.stop();
            updateStatus();
        });
        connect(&helper, &QLocalSocket::errorOccurred, this, [this] {
            helperState = "missing";
            updateStatus();
        });
        heartbeat.setInterval(1000);
        connect(&heartbeat, &QTimer::timeout, this, [this] { sendConfig(); });
        heartbeat.start();
        reconnect.setInterval(2000);
        connect(&reconnect, &QTimer::timeout, this, [this] {
            connectHelper();
            refreshLock();
        });
        reconnect.start();
        scheduler.setInterval(12);
        scheduler.setTimerType(Qt::PreciseTimer);
        connect(&scheduler, &QTimer::timeout, this, [this] {
            NoteQueue::Item n;
            if (queue.pop(clock.elapsed(), n)) {
                if (audio.strike(n.note, n.strength)) {
                    sounded++;
                    activityLabel->setText(QString("%1 little chimes this session").arg(sounded));
                } else
                    activityLabel->setText(audio.error);
            }
            if (queue.items.empty())
                scheduler.stop();
        });
        qDBusRegisterMetaType<Shortcut>();
        qDBusRegisterMetaType<QList<Shortcut>>();
        auto bus = QDBusConnection::sessionBus();
        bus.connect("org.freedesktop.ScreenSaver", "/ScreenSaver", "org.freedesktop.ScreenSaver",
                    "ActiveChanged", this, SLOT(lockChanged(bool)));
        bus.connect("org.freedesktop.portal.Desktop", "", "org.freedesktop.portal.Request",
                    "Response", this, SLOT(portalResponse(uint, QVariantMap, QDBusMessage)));
        bus.connect("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                    "org.freedesktop.portal.GlobalShortcuts", "Activated", this,
                    SLOT(shortcutActivated(QDBusObjectPath, QString, qulonglong, QVariantMap)));
        controlPath = runtimePath();
        QLocalServer::removeServer(controlPath);
        control.setSocketOptions(QLocalServer::UserAccessOption);
        control.listen(controlPath);
        connect(&control, &QLocalServer::newConnection, this, [this] {
            while (control.hasPendingConnections()) {
                auto *s = control.nextPendingConnection();
                connect(s, &QLocalSocket::readyRead, this, [this, s] {
                    auto cmd = s->readAll().trimmed();
                    if (cmd == "show")
                        show();
                    else if (cmd == "pause")
                        setEnabled(false);
                    else if (cmd == "resume")
                        setEnabled(true);
                    else if (cmd == "toggle")
                        setEnabled(!enabled);
                    else if (cmd == "preview")
                        preview();
                    else if (cmd == "shortcut")
                        createShortcut();
                    else if (cmd == "quit") {
                        helper.abort();
                        qApp->quit();
                    }
                    s->write(
                        QString("state=%1 enabled=%2 lock_known=%3 locked=%4 keyboards=%5 "
                                "letters=%6 spacing=%7 chimes=%8 shortcut=%9 audio_error=%10\n")
                            .arg(helperState)
                            .arg(enabled)
                            .arg(lockKnown)
                            .arg(screenLocked)
                            .arg(keyboardCount)
                            .arg(density())
                            .arg(queue.spacing)
                            .arg(sounded)
                            .arg(shortcutDescription)
                            .arg(audio.error)
                            .toUtf8());
                    s->flush();
                    s->disconnectFromServer();
                });
                connect(s, &QLocalSocket::disconnected, s, &QObject::deleteLater);
            }
        });
        refreshLock();
        connectHelper();
        updateStatus();
        if (!trayOnly || !QSystemTrayIcon::isSystemTrayAvailable())
            show();
        if (settings.value("autostart", true).toBool() &&
            QCoreApplication::applicationFilePath().startsWith("/opt/windchime/"))
            setAutostart(true);
        // Portal sessions live only as long as this process. Rebind saved KDE
        // shortcuts each launch so Pause still works after logging in again.
        if (!diagnosticMode &&
            QCoreApplication::applicationFilePath().startsWith("/opt/windchime/"))
            QTimer::singleShot(600, this, [this] { createShortcut(); });
    }
    ~App() override {
        helper.abort();
        audio.stop();
        control.close();
        QLocalServer::removeServer(controlPath);
    }
    static QString socketPath() {
        return runtimePath();
    }
    void renderTo(const QString &path) {
        show();
        QTimer::singleShot(300, this, [this, path] {
            bool ok = window.grab().save(path);
            qApp->exit(ok ? 0 : 1);
        });
    }
    void checkUi() {
        bool ok = true;
        densitySlider->setValue(1);
        ok &= densityValue->text() == "Every letter" && settings.value("letters").toInt() == 1;
        densitySlider->setValue(10);
        ok &= densityValue->text() == "Every 10 letters" && settings.value("letters").toInt() == 10;
        paceSlider->setValue(800);
        ok &= queue.spacing == 800 && paceValue->text() == "800 ms";
        volumeSlider->setValue(100);
        ok &= volumeValue->text() == "100%" && settings.value("volume").toInt() == 100;
        volumeSlider->setValue(0);
        ok &= volumeValue->text() == "0%";
        decaySlider->setValue(60);
        ok &= decayValue->text() == "6.0 s";
        toneBox->setCurrentIndex(2);
        ok &= settings.value("tone").toInt() == 2;
        setEnabled(false);
        ok &= toggleButton->text() == "Resume" && statusLabel->text().startsWith("Paused");
        setEnabled(true);
        ok &= toggleButton->text() == "Pause";
        preview();
        int priorPreview = previewGeneration;
        queue.push(0, .8, clock.elapsed());
        lockChanged(true);
        ok &=
            screenLocked && queue.items.empty() && !previewing && previewGeneration > priorPreview;
        lockChanged(false);
        ok &= !screenLocked && lockKnown;
        window.show();
        window.close();
        ok &= !window.isVisible();
        puts(ok ? "PASS: native sliders, full volume range, settings persistence, tone selection, "
                  "pause/resume, lock handling and close-to-tray."
                : "FAIL: native UI check");
        QTimer::singleShot(0, qApp, [ok] { qApp->exit(ok ? 0 : 1); });
    }
  public slots:
    void lockChanged(bool locked) {
        ++lockRevision;
        lockKnown = true;
        applyLock(locked);
    }
    void shortcutActivated(const QDBusObjectPath &path, const QString &id, qulonglong,
                           const QVariantMap &) {
        if (path.path() == portalSession && id == "pause")
            setEnabled(!enabled);
    }
    void portalResponse(uint code, const QVariantMap &results, const QDBusMessage &message) {
        if (!requestingShortcut || !message.path().contains("windchime_"))
            return;
        if (code != 0) {
            requestingShortcut = false;
            shortcutLabel->setText("Pause shortcut: not assigned · use the tray");
            return;
        }
        if (results.contains("session_handle")) {
            portalSession = results.value("session_handle").toString();
            if (portalSession.isEmpty()) {
                requestingShortcut = false;
                return;
            }
            QList<Shortcut> shortcuts{{"pause",
                                       {{"description", "Pause or resume Windchime"},
                                        {"preferred_trigger", "CTRL+ALT+w"}}}};
            callPortal(
                "BindShortcuts",
                {QVariant::fromValue(QDBusObjectPath(portalSession)),
                 QVariant::fromValue(shortcuts), QString(),
                 QVariantMap{{"handle_token",
                              "windchime_bind_" +
                                  QString::number(QRandomGenerator::global()->generate())}}});
        } else if (results.contains("shortcuts")) {
            auto shortcuts = qdbus_cast<QList<Shortcut>>(results.value("shortcuts"));
            requestingShortcut = false;
            shortcutDescription = shortcuts.isEmpty()
                                      ? "Not assigned"
                                      : shortcuts.front()
                                            .options.value("trigger_description", "Ctrl+Alt+W")
                                            .toString();
            shortcutLabel->setText("Pause shortcut: " + shortcutDescription);
        }
    }
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    app.setApplicationName("Windchime");
    app.setOrganizationName("Windchime");
    app.setDesktopFileName("windchime");
    app.setApplicationVersion("1.0.0");
    app.setQuitOnLastWindowClosed(false);
    QStringList args = app.arguments();
    QString command = "show";
    for (auto s : {"pause", "resume", "toggle", "quit", "status", "preview", "shortcut"})
        if (args.contains("--" + QString(s)))
            command = s;
    QLocalSocket existing;
    existing.connectToServer(App::socketPath());
    if (existing.waitForConnected(250)) {
        existing.write(command.toUtf8() + "\n");
        existing.flush();
        existing.waitForBytesWritten(200);
        if (existing.waitForReadyRead(1500))
            fputs(existing.readAll().constData(), stdout);
        return 0;
    }
    if (command != "show" && command != "preview") {
        fprintf(stderr, "Windchime is not running.\n");
        return 1;
    }
    QLockFile instanceLock(App::socketPath() + ".lock");
    instanceLock.setStaleLockTime(0);
    if (!instanceLock.tryLock(0)) {
        fprintf(stderr, "Windchime is starting or already running.\n");
        return 1;
    }
    App windchime(args.contains("--tray"));
    int render = args.indexOf("--render");
    if (render >= 0 && render + 1 < args.size())
        windchime.renderTo(args[render + 1]);
    if (args.contains("--ui-check"))
        windchime.checkUi();
    return app.exec();
}
#include "main.moc"
