// SPDX-License-Identifier: MIT
/*
 * CEF powered webview plugin
 *
 * Implements the platform side of the `webview_cef` pub package
 * (https://pub.dev/packages/webview_cef), so flutter apps can embed web pages
 * on flutter-pi with the same dart code they use on desktop.
 *
 * Pages are rendered off-screen by the Chromium Embedded Framework and handed to
 * flutter as external textures, since flutter-pi has no platform views.
 *
 * See src/plugins/webview_cef/README.md.
 *
 * Copyright (c) 2026, Bojidar Tonchev <bojidar.tonchev@gmail.com>
 */

#ifndef _FLUTTERPI_SRC_PLUGINS_WEBVIEW_CEF_H
#define _FLUTTERPI_SRC_PLUGINS_WEBVIEW_CEF_H

#define WEBVIEW_CEF_CHANNEL "webview_cef"

#endif  // _FLUTTERPI_SRC_PLUGINS_WEBVIEW_CEF_H
