/******************************************************************************
 *
 * Copyright (C) 2019 Marton Borzak <hello@martonborzak.com>
 *
 * This file is part of the YIO-Remote software project.
 *
 * YIO-Remote software is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * YIO-Remote software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with YIO-Remote software. If not, see <https://www.gnu.org/licenses/>.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *****************************************************************************/

#pragma once

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>

#include <QSysInfo>

#include "yio-interface/entities/mediaplayerinterface.h"
#include "yio-model/mediaplayer/albummodel_mediaplayer.h"
#include "yio-model/mediaplayer/searchmodel_mediaplayer.h"
#include "yio-plugin/integration.h"
#include "yio-plugin/plugin.h"

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//// PLEXMEDIA FACTORY
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

const bool USE_WORKER_THREAD = false;

class PlexMediaPlugin : public Plugin {
    Q_OBJECT
    Q_INTERFACES(PluginInterface)
    Q_PLUGIN_METADATA(IID "YIO.PluginInterface" FILE "plexmedia.json")

 public:
    PlexMediaPlugin();

    // Plugin interface
 protected:
    Integration* createIntegration(const QVariantMap& config, EntitiesInterface* entities,
                                   NotificationsInterface* notifications, YioAPIInterface* api,
                                   ConfigInterface* configObj) override;
};

////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//// PLEXMEDIA CLASS
////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

class PlexMedia : public Integration {
    Q_OBJECT

 public:
    explicit PlexMedia(const QVariantMap& config, EntitiesInterface* entities, NotificationsInterface* notifications,
                     YioAPIInterface* api, ConfigInterface* configObj, Plugin* plugin);

    void sendCommand(const QString& type, const QString& entitId, int command, const QVariant& param) override;

 public slots:  // NOLINT open issue: https://github.com/cpplint/cpplint/pull/99
    void connect() override;
    void disconnect() override;
    void enterStandby() override;
    void leaveStandby() override;

 signals:
    void requestReady(const QVariantMap& obj, const QString& url);

 private:
    // PlexMedia API calls
    void search(QString query);
    void search(QString query, QString type);
    void getAlbum(QString id);
    void getPlaylist(QString id);
    void getUserPlaylists();

    // PlexMedia API authentication
    void getMachineIdentifier();
    void requestAuthToken();

    // PlexMedia status API calls
    void getCurrentPlayer();  //subsribtion option is possible but not advisable as connection is not kept open.

    void updateEntity(const QString& entity_id, const QVariantMap& attr);
    void updateBrowseModel(BrowseModel * model);

    // get and post requests
    void getRequest(const QString& url, const QString& params);
    void postRequest(const QString& url, const QString& params);
    void putRequest(const QString& url, const QString& params);  // TODO(marton): change param to QUrlQuery
                                                                 // QUrlQuery query;

    void getPollRequest(const QString& url, const QString& params);  //returns player info from /client endpoint in XML format

    // speaker/source selection
    void changeSpeaker(const QString& id);  //change the speaker/source
    void getSpeakers(const QVariantMap& map);  //returns model populated with speakers/sources

 private slots:  // NOLINT open issue: https://github.com/cpplint/cpplint/pull/99
    void onPollingTimerTimeout();

 private:
    bool    m_speakerRequest = true;
    QString m_entityId;

    // polling timer
    QTimer* m_pollingTimer;

    // PMS details
    QString m_serverIP;
    QString m_serverPort;
    QString m_serverURL;
    QString m_serverId;

    // Yio details
    QByteArray m_remoteId = QSysInfo::machineUniqueId();
    QByteArray m_remoteSys = "yioRemote"; //OS name
    QByteArray m_remoteName = "My YIO Remote"; //Device name

    // Player details
    QString m_playerId;
    QString m_playerIP;
    QString m_playerPort = "0"; //set as 0 for default (no info)
    QString m_playerURL;
    QString m_playerPlatform; //used to track issue and bugs with different platforms. I.e. iOS catastropically crashes out on refreshPlayQueue request (as of 18/05/2020).
    QString m_playerQueue; //now playing queue Id
    QString m_playerCurrentTrack = "0"; //store current track to reduce polling burden. Set as 0 for default (no info)
    QString m_playerState;
    int  m_playerDuration;
    int  m_playerTime;
    int  m_playerVol = 100; //track volume, default to max
    bool m_playerConnected = false;
    bool m_directConn = true;
    bool m_newTrack = true;
    int  m_cmdId = 0; //cmdId is used by Plex to track the order of requests

    // Plex auth
    QString m_clientUser;
    QString m_clientPass;
    QString m_authToken;
};
