#pragma once

#include <QObject>
#include <QJsonObject>

struct QDiscordMessage {
Q_GADGET

public:
	enum class MessageType {
		unkonwn,
		ready,
		error,
		guildStatus,
		guildCreate,
		channelCreate,
		voiceChannelSelect,
		voiceStateCreate,
		voiceStateUpdate,
		voiceStateDelete,
		voiceSettingsUpdate,
		voiceConnectionStatus,
		speakingStart,
		speakingStop,
		messageCreate,
		messageUpdate,
		messageDelete,
		notificationCreate,
		activityJoin,
		activitySpectate,
		activityJoinRequest,
	};

	Q_ENUM(MessageType);

public:
	static QDiscordMessage fromJson(const QJsonObject &json, int opcode = 0);

public:
	MessageType type = MessageType::unkonwn;
	QJsonObject json;
	QString nonce;
	int opcode = 0;

};
