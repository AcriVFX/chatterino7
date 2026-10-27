// SPDX-FileCopyrightText: 2018 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#pragma once

class QString;

namespace chatterino {

class Paths;

/// `origin` is the browser extension that started the host
/// (chrome-extension://<id>/), or the manifest path on Firefox
void runBrowserExtensionHost(const Paths &paths, const QString &origin);

}  // namespace chatterino
