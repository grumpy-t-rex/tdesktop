/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#pragma once

#include "export/output/export_output_abstract.h"
#include "export/output/export_output_file.h"
#include "export/export_settings.h"
#include "export/data/export_data_types.h"

namespace Export {
namespace Output {
namespace details {

class HtmlContext {
public:
	[[nodiscard]] QByteArray pushTag(
		const QByteArray &tag,
		std::map<QByteArray, QByteArray> &&attributes = {});
	[[nodiscard]] QByteArray popTag();
	[[nodiscard]] QByteArray indent() const;
	[[nodiscard]] bool empty() const;

private:
	struct Tag {
		QByteArray name;
		bool block = true;
	};
	std::vector<Tag> _tags;

};

struct UserpicData;
class PeersMap;
struct MediaData;

} // namespace details

} // namespace Output
} // namespace Export
