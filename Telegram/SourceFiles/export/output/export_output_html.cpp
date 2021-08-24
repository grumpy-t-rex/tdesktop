/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "export/output/export_output_html.h"

#include "export/output/export_output_result.h"
#include "export/data/export_data_types.h"
#include "core/utils.h"

#include <QtCore/QSize>
#include <QtCore/QFile>
#include <QtCore/QDateTime>

namespace Export {
namespace Output {
namespace {

constexpr auto kMessagesInFile = 1000;
constexpr auto kPersonalUserpicSize = 90;
constexpr auto kEntryUserpicSize = 48;
constexpr auto kServiceMessagePhotoSize = 60;
constexpr auto kHistoryUserpicSize = 42;
constexpr auto kSavedMessagesColorIndex = 3;
constexpr auto kJoinWithinSeconds = 900;
constexpr auto kPhotoMaxWidth = 520;
constexpr auto kPhotoMaxHeight = 520;
constexpr auto kPhotoMinWidth = 80;
constexpr auto kPhotoMinHeight = 80;
constexpr auto kStickerMaxWidth = 384;
constexpr auto kStickerMaxHeight = 384;
constexpr auto kStickerMinWidth = 80;
constexpr auto kStickerMinHeight = 80;

const auto kLineBreak = QByteArrayLiteral("<br>");

using UserpicData = details::UserpicData;
using PeersMap = details::PeersMap;
using MediaData = details::MediaData;

bool IsGlobalLink(const QString &link) {
	return link.startsWith(qstr("http://"), Qt::CaseInsensitive)
		|| link.startsWith(qstr("https://"), Qt::CaseInsensitive);
}

QByteArray NoFileDescription(Data::File::SkipReason reason) {
	using SkipReason = Data::File::SkipReason;
	switch (reason) {
	case SkipReason::Unavailable:
		return "Unavailable, please try again later.";
	case SkipReason::FileSize:
		return "Exceeds maximum size, "
			"change data exporting settings to download.";
	case SkipReason::FileType:
		return "Not included, "
			"change data exporting settings to download.";
	case SkipReason::None:
		return "";
	}
	Unexpected("Skip reason in NoFileDescription.");
}

auto CalculateThumbSize(
		int maxWidth,
		int maxHeight,
		int minWidth,
		int minHeight,
		bool expandForRetina = false) {
	return [=](QSize largeSize) {
		const auto multiplier = (expandForRetina ? 2 : 1);
		const auto checkWidth = largeSize.width() * multiplier;
		const auto checkHeight = largeSize.height() * multiplier;
		const auto smallSize = (checkWidth > maxWidth
			|| checkHeight > maxHeight)
			? largeSize.scaled(
				maxWidth,
				maxHeight,
				Qt::KeepAspectRatio)
			: largeSize;
		const auto retinaSize = QSize(
			smallSize.width() & ~0x01,
			smallSize.height() & ~0x01);
		return (retinaSize.width() < kPhotoMinWidth
			|| retinaSize.height() < kPhotoMinHeight)
			? QSize()
			: retinaSize;
	};
}

QByteArray SerializeString(const QByteArray &value) {
	const auto size = value.size();
	const auto begin = value.data();
	const auto end = begin + size;

	auto result = QByteArray();
	result.reserve(size * 6);
	for (auto p = begin; p != end; ++p) {
		const auto ch = *p;
		if (ch == '\n') {
			result.append("<br>", 4);
		} else if (ch == '"') {
			result.append("&quot;", 6);
		} else if (ch == '&') {
			result.append("&amp;", 5);
		} else if (ch == '\'') {
			result.append("&apos;", 6);
		} else if (ch == '<') {
			result.append("&lt;", 4);
		} else if (ch == '>') {
			result.append("&gt;", 4);
		} else if (ch >= 0 && ch < 32) {
			result.append("&#x", 3).append('0' + (ch >> 4));
			const auto left = (ch & 0x0F);
			if (left >= 10) {
				result.append('A' + (left - 10));
			} else {
				result.append('0' + left);
			}
			result.append(';');
		} else if (ch == char(0xE2)
			&& (p + 2 < end)
			&& *(p + 1) == char(0x80)) {
			if (*(p + 2) == char(0xA8)) { // Line separator.
				result.append("<br>", 4);
			} else if (*(p + 2) == char(0xA9)) { // Paragraph separator.
				result.append("<br>", 4);
			} else {
				result.append(ch);
			}
		} else {
			result.append(ch);
		}
	}
	return result;
}

QByteArray SerializeList(const std::vector<QByteArray> &values) {
	const auto count = values.size();
	if (count == 1) {
		return values[0];
	} else if (count > 1) {
		auto result = values[0];
		for (auto i = 1; i != count - 1; ++i) {
			result += ", " + values[i];
		}
		return result + " and " + values[count - 1];
	}
	return QByteArray();
}
QByteArray MakeLinks(const QByteArray &value) {
	const auto domain = QByteArray("https://telegram.org/");
	auto result = QByteArray();
	auto offset = 0;
	while (true) {
		const auto start = value.indexOf(domain, offset);
		if (start < 0) {
			break;
		}
		auto end = start + domain.size();
		for (; end != value.size(); ++end) {
			const auto ch = value[end];
			if ((ch < 'a' || ch > 'z')
				&& (ch < 'A' || ch > 'Z')
				&& (ch < '0' || ch > '9')
				&& (ch != '-')
				&& (ch != '_')
				&& (ch != '/')) {
				break;
			}
		}
		if (start > offset) {
			const auto link = value.mid(start, end - start);
			result.append(value.mid(offset, start - offset));
			result.append("<a href=\"").append(link).append("\">");
			result.append(link);
			result.append("</a>");
			offset = end;
		}
	}
	if (result.isEmpty()) {
		return value;
	}
	if (offset < value.size()) {
		result.append(value.mid(offset));
	}
	return result;
}

void SerializeMultiline(
		QByteArray &appendTo,
		const QByteArray &value,
		int newline) {
	const auto data = value.data();
	auto offset = 0;
	do {
		appendTo.append("> ");
		const auto win = (newline > 0 && *(data + newline - 1) == '\r');
		if (win) --newline;
		appendTo.append(data + offset, newline - offset).append(kLineBreak);
		if (win) ++newline;
		offset = newline + 1;
		newline = value.indexOf('\n', offset);
	} while (newline > 0);
	if (const auto size = value.size(); size > offset) {
		appendTo.append("> ");
		appendTo.append(data + offset, size - offset).append(kLineBreak);
	}
}

QByteArray JoinList(
		const QByteArray &separator,
		const std::vector<QByteArray> &list) {
	if (list.empty()) {
		return QByteArray();
	} else if (list.size() == 1) {
		return list[0];
	}
	auto size = (list.size() - 1) * separator.size();
	for (const auto &value : list) {
		size += value.size();
	}
	auto result = QByteArray();
	result.reserve(size);
	auto counter = 0;
	while (true) {
		result.append(list[counter]);
		if (++counter == list.size()) {
			break;
		} else {
			result.append(separator);
		}
	}
	return result;
}

QByteArray FormatText(
		const std::vector<Data::TextPart> &data,
		const QString &internalLinksDomain) {
	return JoinList(QByteArray(), ranges::view::all(
		data
	) | ranges::view::transform([&](const Data::TextPart &part) {
		const auto text = SerializeString(part.text);
		using Type = Data::TextPart::Type;
		switch (part.type) {
		case Type::Text: return text;
		case Type::Unknown: return text;
		case Type::Mention:
			return "<a href=\""
				+ internalLinksDomain.toUtf8()
				+ text.mid(1)
				+ "\">" + text + "</a>";
		case Type::Hashtag: return "<a href=\"\" "
			"onclick=\"return ShowHashtag("
			+ SerializeString('"' + text.mid(1) + '"')
			+ ")\">" + text + "</a>";
		case Type::BotCommand: return "<a href=\"\" "
			"onclick=\"return ShowBotCommand("
			+ SerializeString('"' + text.mid(1) + '"')
			+ ")\">" + text + "</a>";
		case Type::Url: return "<a href=\""
			+ text
			+ "\">" + text + "</a>";
		case Type::Email: return "<a href=\"mailto:"
			+ text
			+ "\">" + text + "</a>";
		case Type::Bold: return "<strong>" + text + "</strong>";
		case Type::Italic: return "<em>" + text + "</em>";
		case Type::Code: return "<code>" + text + "</code>";
		case Type::Pre: return "<pre>" + text + "</pre>";
		case Type::TextUrl: return "<a href=\""
			+ SerializeString(part.additional)
			+ "\">" + text + "</a>";
		case Type::MentionName: return "<a href=\"\" "
			"onclick=\"return ShowMentionName()\">" + text + "</a>";
		case Type::Phone: return "<a href=\"tel:"
			+ text
			+ "\">" + text + "</a>";
		case Type::Cashtag: return "<a href=\"\" "
			"onclick=\"return ShowCashtag("
			+ SerializeString('"' + text.mid(1) + '"')
			+ ")\">" + text + "</a>";
		case Type::Underline: return "<u>" + text + "</u>";
		case Type::Strike: return "<s>" + text + "</s>";
		case Type::Blockquote:
			return "<blockquote>" + text + "</blockquote>";
		case Type::BankCard:
			return text;
		}
		Unexpected("Type in text entities serialization.");
	}) | ranges::to_vector);
}

QByteArray SerializeKeyValue(
		std::vector<std::pair<QByteArray, QByteArray>> &&values) {
	auto result = QByteArray();
	for (const auto &[key, value] : values) {
		if (value.isEmpty()) {
			continue;
		}
		result.append(key);
		if (const auto newline = value.indexOf('\n'); newline >= 0) {
			result.append(':').append(kLineBreak);
			SerializeMultiline(result, value, newline);
		} else {
			result.append(": ").append(value).append(kLineBreak);
		}
	}
	return result;
}

QByteArray SerializeBlockquote(
		std::vector<std::pair<QByteArray, QByteArray>> &&values) {
	return "<blockquote>"
		+ SerializeKeyValue(std::move(values))
		+ "</blockquote>";
}

Data::Utf8String FormatUsername(const Data::Utf8String &username) {
	return username.isEmpty() ? username : ('@' + username);
}

bool DisplayDate(TimeId date, TimeId previousDate) {
	if (!previousDate) {
		return true;
	}
	return QDateTime::fromTime_t(date).date()
		!= QDateTime::fromTime_t(previousDate).date();
}

QByteArray FormatDateText(TimeId date) {
	const auto parsed = QDateTime::fromTime_t(date).date();
	const auto month = [](int index) {
		switch (index) {
		case 1: return "January";
		case 2: return "February";
		case 3: return "March";
		case 4: return "April";
		case 5: return "May";
		case 6: return "June";
		case 7: return "July";
		case 8: return "August";
		case 9: return "September";
		case 10: return "October";
		case 11: return "November";
		case 12: return "December";
		}
		return "Unknown";
	};
	return Data::NumberToString(parsed.day())
		+ ' '
		+ month(parsed.month())
		+ ' '
		+ Data::NumberToString(parsed.year());
}

QByteArray FormatTimeText(TimeId date) {
	const auto parsed = QDateTime::fromTime_t(date).time();
	return Data::NumberToString(parsed.hour(), 2)
		+ ':'
		+ Data::NumberToString(parsed.minute(), 2);
}

QByteArray SerializeLink(
		const Data::Utf8String &text,
		const QString &path) {
	return "<a href=\"" + path.toUtf8() + "\">" + text + "</a>";
}

} // namespace

namespace details {

struct UserpicData {
	int colorIndex = 0;
	int pixelSize = 0;
	QString imageLink;
	QString largeLink;
	QByteArray firstName;
	QByteArray lastName;
};

class PeersMap {
public:
	using PeerId = Data::PeerId;
	using Peer = Data::Peer;
	using User = Data::User;
	using Chat = Data::Chat;

	PeersMap(const std::map<PeerId, Peer> &data);

	const Peer &peer(PeerId peerId) const;
	const User &user(int32 userId) const;
	const Chat &chat(int32 chatId) const;

	QByteArray wrapPeerName(PeerId peerId) const;
	QByteArray wrapUserName(int32 userId) const;
	QByteArray wrapUserNames(const std::vector<int32> &data) const;

private:
	const std::map<Data::PeerId, Data::Peer> &_data;

};

struct MediaData {
	QByteArray title;
	QByteArray description;
	QByteArray status;
	QByteArray classes;
	QString thumb;
	QString link;
};

PeersMap::PeersMap(const std::map<PeerId, Peer> &data) : _data(data) {
}

auto PeersMap::peer(PeerId peerId) const -> const Peer & {
	if (const auto i = _data.find(peerId); i != end(_data)) {
		return i->second;
	}
	static auto empty = Peer{ User() };
	return empty;
}

auto PeersMap::user(int32 userId) const -> const User & {
	if (const auto result = peer(Data::UserPeerId(userId)).user()) {
		return *result;
	}
	static auto empty = User();
	return empty;
}

auto PeersMap::chat(int32 chatId) const -> const Chat & {
	if (const auto result = peer(Data::ChatPeerId(chatId)).chat()) {
		return *result;
	}
	static auto empty = Chat();
	return empty;
}

QByteArray PeersMap::wrapPeerName(PeerId peerId) const {
	const auto result = peer(peerId).name();
	return result.isEmpty()
		? QByteArray("Deleted")
		: SerializeString(result);
}

QByteArray PeersMap::wrapUserName(int32 userId) const {
	const auto result = user(userId).name();
	return result.isEmpty()
		? QByteArray("Deleted Account")
		: SerializeString(result);
}

QByteArray PeersMap::wrapUserNames(const std::vector<int32> &data) const {
	auto list = std::vector<QByteArray>();
	for (const auto userId : data) {
		list.push_back(wrapUserName(userId));
	}
	return SerializeList(list);
}

} // namespace details




void FillUserpicNames(UserpicData &data, const Data::Peer &peer) {
	if (peer.user()) {
		data.firstName = peer.user()->info.firstName;
		data.lastName = peer.user()->info.lastName;
	} else if (peer.chat()) {
		data.firstName = peer.name();
	}
}

void FillUserpicNames(UserpicData &data, const QByteArray &full) {
	const auto names = full.split(' ');
	data.firstName = names[0];
	for (auto i = 1; i != names.size(); ++i) {
		if (names[i].isEmpty()) {
			continue;
		}
		if (!data.lastName.isEmpty()) {
			data.lastName.append(' ');
		}
		data.lastName.append(names[i]);
	}
}

QByteArray ComposeName(const UserpicData &data, const QByteArray &empty) {
	return ((data.firstName.isEmpty() && data.lastName.isEmpty())
		? empty
		: (data.firstName + ' ' + data.lastName));
}

QString WriteUserpicThumb(
		const QString &basePath,
		const QString &largePath,
		const UserpicData &userpic,
		const QString &postfix = "_thumb") {
	return Data::WriteImageThumb(
		basePath,
		largePath,
		userpic.pixelSize * 2,
		userpic.pixelSize * 2,
		postfix);
}

} // namespace Output
} // namespace Export
