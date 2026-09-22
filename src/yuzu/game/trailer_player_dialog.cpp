// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "yuzu/game/trailer_player_dialog.h"

#include <filesystem>

#include <QHBoxLayout>
#include <QCloseEvent>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QPointer>
#include <QResizeEvent>
#include <QSaveFile>
#include <QShowEvent>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "hid_core/hid_core.h"
#include "yuzu/util/controller_navigation.h"

#ifdef YUZU_USE_WEBVIEW2
#include <Windows.h>
#include <Unknwn.h>
#include <wrl.h>
#include <WebView2.h>
#if defined(_MSC_VER)
#pragma push_macro("__has_attribute")
#undef __has_attribute
#endif
#include <WebView2EnvironmentOptions.h>
#if defined(_MSC_VER)
#pragma pop_macro("__has_attribute")
#endif
#endif

#ifdef YUZU_USE_WEBVIEW2
namespace {

constexpr char PLAYER_HTML[] = R"html(<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <meta name="referrer" content="strict-origin-when-cross-origin">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    html, body, iframe { width: 100%; height: 100%; margin: 0; border: 0; overflow: hidden; background: #000; }
  </style>
</head>
<body>
  <iframe id="player" title="YouTube trailer" allow="autoplay; encrypted-media; picture-in-picture" allowfullscreen></iframe>
  <script>
    const videoId = new URLSearchParams(window.location.search).get('video');
    const origin = encodeURIComponent(window.location.origin);
    const referrer = encodeURIComponent(window.location.href);
    document.getElementById('player').src =
      'https://www.youtube-nocookie.com/embed/' + encodeURIComponent(videoId) +
      '?autoplay=1&controls=1&rel=0&playsinline=1&fs=1&origin=' + origin +
      '&widget_referrer=' + referrer;
  </script>
</body>
</html>)html";

bool PreparePlayerContent(std::filesystem::path& content_directory) {
    content_directory =
        Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "youtube_webview2_content";
    if (!Common::FS::CreateDirs(content_directory)) {
        return false;
    }

    QSaveFile player_file{
        QString::fromStdWString((content_directory / "trailer_player.html").wstring())};
    if (!player_file.open(QIODevice::WriteOnly) ||
        player_file.write(PLAYER_HTML, sizeof(PLAYER_HTML) - 1) != sizeof(PLAYER_HTML) - 1) {
        return false;
    }
    return player_file.commit();
}

} // Anonymous namespace
#endif

struct TrailerPlayerDialog::Impl {
#ifdef YUZU_USE_WEBVIEW2
    Microsoft::WRL::ComPtr<ICoreWebView2EnvironmentOptions> environment_options;
    Microsoft::WRL::ComPtr<ICoreWebView2Environment> environment;
    Microsoft::WRL::ComPtr<ICoreWebView2Controller> controller;
    Microsoft::WRL::ComPtr<ICoreWebView2> webview;
    EventRegistrationToken new_window_requested_token{};
    bool has_new_window_requested_handler{};
#endif
};

TrailerPlayerDialog::TrailerPlayerDialog(const QString& video_id_, const QString& title,
                                         Core::HID::HIDCore& hid_core, QWidget* parent)
    : QDialog{parent, Qt::Dialog | Qt::FramelessWindowHint}, impl{std::make_unique<Impl>()},
      video_id{video_id_} {
    setWindowModality(Qt::ApplicationModal);
    setWindowTitle(title);
    setMinimumSize(640, 396);
    resize(960, 576);
    setStyleSheet(QStringLiteral("QDialog { background: #000000; }"));

    auto* root_layout = new QVBoxLayout(this);
    root_layout->setContentsMargins(0, 0, 0, 0);
    root_layout->setSpacing(0);

    auto* title_bar = new QWidget(this);
    title_bar->setFixedHeight(36);
    title_bar->setStyleSheet(QStringLiteral("background: #151515; color: white;"));
    auto* title_layout = new QHBoxLayout(title_bar);
    title_layout->setContentsMargins(12, 0, 4, 0);
    title_label = new QLabel(title, title_bar);
    title_label->setTextInteractionFlags(Qt::NoTextInteraction);
    title_layout->addWidget(title_label, 1);

    auto* close_button = new QToolButton(title_bar);
    close_button->setText(QStringLiteral("\u2715"));
    close_button->setToolTip(tr("Close trailer"));
    close_button->setFixedSize(32, 28);
    close_button->setStyleSheet(QStringLiteral(
        "QToolButton { color: white; border: none; font-size: 16px; }"
        "QToolButton:hover { background: #c42b1c; }"));
    connect(close_button, &QToolButton::clicked, this, &TrailerPlayerDialog::ClosePlayer);
    title_layout->addWidget(close_button);
    root_layout->addWidget(title_bar);

    player_container = new QWidget(this);
    player_container->setAttribute(Qt::WA_NativeWindow);
    player_container->setStyleSheet(QStringLiteral("background: #000000;"));
    auto* player_layout = new QVBoxLayout(player_container);
    player_layout->setContentsMargins(0, 0, 0, 0);
    loading_label = new QLabel(tr("Loading trailer..."), player_container);
    loading_label->setAlignment(Qt::AlignCenter);
    loading_label->setStyleSheet(QStringLiteral("color: white; font-size: 16px;"));
    player_layout->addWidget(loading_label);
    root_layout->addWidget(player_container, 1);

    controller_navigation = new ControllerNavigation(hid_core, this, true);
    connect(controller_navigation, &ControllerNavigation::TriggerKeyboardEvent, this,
            [this](Qt::Key key) {
                if (isVisible() && key == Qt::Key_Escape) {
                    ClosePlayer();
                }
            });

    Play(video_id_, title);
}

TrailerPlayerDialog::~TrailerPlayerDialog() {
#ifdef YUZU_USE_WEBVIEW2
    if (impl->webview && impl->has_new_window_requested_handler) {
        impl->webview->remove_NewWindowRequested(impl->new_window_requested_token);
    }
    if (impl->controller) {
        impl->controller->Close();
    }
    impl->webview.Reset();
    impl->controller.Reset();
    impl->environment.Reset();
    impl->environment_options.Reset();
#endif
}

void TrailerPlayerDialog::Play(const QString& video_id_, const QString& title) {
    video_id = video_id_;
    setWindowTitle(title);
    title_label->setText(title);
    playback_requested = true;

    if (unavailable) {
        QTimer::singleShot(0, this, &TrailerPlayerDialog::FailPlayback);
        return;
    }
    if (initialized) {
        if (isVisible()) {
            QTimer::singleShot(0, this, &TrailerPlayerDialog::NavigatePlayer);
        }
        return;
    }
    if (!initializing) {
        QTimer::singleShot(0, this, &TrailerPlayerDialog::InitializePlayer);
    }
}

void TrailerPlayerDialog::closeEvent(QCloseEvent* event) {
    StopPlayback();
    QDialog::closeEvent(event);
}

void TrailerPlayerDialog::hideEvent(QHideEvent* event) {
#ifdef YUZU_USE_WEBVIEW2
    if (impl->controller) {
        impl->controller->put_IsVisible(FALSE);
    }
#endif
    QDialog::hideEvent(event);
}

void TrailerPlayerDialog::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        ClosePlayer();
        return;
    }
    QDialog::keyPressEvent(event);
}

void TrailerPlayerDialog::resizeEvent(QResizeEvent* event) {
    QDialog::resizeEvent(event);
    UpdatePlayerBounds();
}

void TrailerPlayerDialog::showEvent(QShowEvent* event) {
    QDialog::showEvent(event);
#ifdef YUZU_USE_WEBVIEW2
    if (impl->controller) {
        impl->controller->put_IsVisible(TRUE);
    }
#endif
    UpdatePlayerBounds();
    if (initialized && playback_requested) {
        QTimer::singleShot(0, this, &TrailerPlayerDialog::NavigatePlayer);
    }
}

void TrailerPlayerDialog::InitializePlayer() {
#ifdef YUZU_USE_WEBVIEW2
    if (initializing || initialized || unavailable) {
        return;
    }
    initializing = true;

    std::filesystem::path content_directory;
    if (!PreparePlayerContent(content_directory)) {
        initializing = false;
        unavailable = true;
        FailPlayback();
        return;
    }

    const auto guard = QPointer<TrailerPlayerDialog>{this};
    const HWND parent_window = reinterpret_cast<HWND>(player_container->winId());
    const std::wstring user_data_directory =
        (Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "youtube_webview2").wstring();
    const auto environment_options =
        Microsoft::WRL::Make<CoreWebView2EnvironmentOptions>();
    if (!environment_options ||
        FAILED(environment_options->put_AdditionalBrowserArguments(
            L"--autoplay-policy=no-user-gesture-required"))) {
        initializing = false;
        unavailable = true;
        FailPlayback();
        return;
    }
    if (FAILED(environment_options.As(&impl->environment_options))) {
        initializing = false;
        unavailable = true;
        FailPlayback();
        return;
    }
    const HRESULT result = CreateCoreWebView2EnvironmentWithOptions(
        nullptr, user_data_directory.c_str(), impl->environment_options.Get(),
        Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [guard, parent_window,
             content_directory](HRESULT environment_result,
                                ICoreWebView2Environment* environment) -> HRESULT {
                if (!guard) {
                    return S_OK;
                }
                if (FAILED(environment_result) || environment == nullptr) {
                    guard->initializing = false;
                    guard->unavailable = true;
                    guard->FailPlayback();
                    return S_OK;
                }
                guard->impl->environment = environment;
                const HRESULT controller_creation_result = environment->CreateCoreWebView2Controller(
                    parent_window,
                    Microsoft::WRL::Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [guard, content_directory](HRESULT controller_result,
                                                   ICoreWebView2Controller* controller) -> HRESULT {
                            if (!guard) {
                                return S_OK;
                            }
                            if (FAILED(controller_result) || controller == nullptr) {
                                guard->initializing = false;
                                guard->unavailable = true;
                                guard->FailPlayback();
                                return S_OK;
                            }

                            guard->impl->controller = controller;
                            controller->get_CoreWebView2(&guard->impl->webview);
                            if (!guard->impl->webview) {
                                guard->initializing = false;
                                guard->unavailable = true;
                                guard->FailPlayback();
                                return S_OK;
                            }

                            Microsoft::WRL::ComPtr<ICoreWebView2Settings> settings;
                            guard->impl->webview->get_Settings(&settings);
                            if (settings) {
                                settings->put_AreDefaultContextMenusEnabled(FALSE);
                                settings->put_AreDevToolsEnabled(FALSE);
                                settings->put_IsStatusBarEnabled(FALSE);
                                settings->put_IsZoomControlEnabled(FALSE);
                            }

                            Microsoft::WRL::ComPtr<ICoreWebView2_3> webview3;
                            if (FAILED(guard->impl->webview.As(&webview3)) ||
                                FAILED(webview3->SetVirtualHostNameToFolderMapping(
                                    L"eden-emu.dev", content_directory.c_str(),
                                    COREWEBVIEW2_HOST_RESOURCE_ACCESS_KIND_DENY_CORS))) {
                                guard->initializing = false;
                                guard->unavailable = true;
                                guard->FailPlayback();
                                return S_OK;
                            }

                            const HRESULT popup_result =
                                guard->impl->webview->add_NewWindowRequested(
                                    Microsoft::WRL::Callback<
                                        ICoreWebView2NewWindowRequestedEventHandler>(
                                        [](ICoreWebView2*,
                                           ICoreWebView2NewWindowRequestedEventArgs* args)
                                            -> HRESULT {
                                            args->put_Handled(TRUE);
                                            return S_OK;
                                        })
                                        .Get(),
                                    &guard->impl->new_window_requested_token);
                            guard->impl->has_new_window_requested_handler =
                                SUCCEEDED(popup_result);

                            guard->initializing = false;
                            guard->initialized = true;
                            guard->UpdatePlayerBounds();
                            guard->loading_label->hide();
                            guard->NavigatePlayer();
                            return S_OK;
                        })
                        .Get());
                if (FAILED(controller_creation_result)) {
                    guard->initializing = false;
                    guard->unavailable = true;
                    guard->FailPlayback();
                }
                return S_OK;
            })
            .Get());
    if (FAILED(result)) {
        initializing = false;
        unavailable = true;
        FailPlayback();
    }
#else
    unavailable = true;
    FailPlayback();
#endif
}

void TrailerPlayerDialog::NavigatePlayer() {
#ifdef YUZU_USE_WEBVIEW2
    if (!playback_requested || !impl->webview) {
        return;
    }
    const QString encoded_video_id = QString::fromLatin1(QUrl::toPercentEncoding(video_id));
    const QString player_url =
        QStringLiteral("https://eden-emu.dev/trailer_player.html?video=%1")
            .arg(encoded_video_id);
    if (FAILED(impl->webview->Navigate(reinterpret_cast<LPCWSTR>(player_url.utf16())))) {
        unavailable = true;
        FailPlayback();
    }
#endif
}

void TrailerPlayerDialog::UpdatePlayerBounds() {
#ifdef YUZU_USE_WEBVIEW2
    if (!impl->controller || !player_container) {
        return;
    }
    const RECT bounds{0, 0, player_container->width(), player_container->height()};
    impl->controller->put_Bounds(bounds);
#endif
}

void TrailerPlayerDialog::ClosePlayer() {
    StopPlayback();
    hide();
}

void TrailerPlayerDialog::StopPlayback() {
    playback_requested = false;
#ifdef YUZU_USE_WEBVIEW2
    if (impl->webview) {
        impl->webview->Navigate(L"about:blank");
    }
#endif
}

void TrailerPlayerDialog::FailPlayback() {
    if (!playback_requested) {
        return;
    }
    const QString failed_video_id = video_id;
    ClosePlayer();
    emit PlaybackFailed(failed_video_id);
}
