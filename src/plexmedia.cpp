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

#include "plexmedia.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <QtXml/QDomDocument>

PlexMediaPlugin::PlexMediaPlugin() : Plugin("plexmedia", USE_WORKER_THREAD) {}

Integration* PlexMediaPlugin::createIntegration(const QVariantMap& config, EntitiesInterface* entities,
                                              NotificationsInterface* notifications, YioAPIInterface* api,
                                              ConfigInterface* configObj) {
    qCInfo(m_logCategory) << "Creating Plex Media integration plugin" << PLUGIN_VERSION;

    return new PlexMedia(config, entities, notifications, api, configObj, this);
}

PlexMedia::PlexMedia(const QVariantMap& config, EntitiesInterface* entities, NotificationsInterface* notifications,
                 YioAPIInterface* api, ConfigInterface* configObj, Plugin* plugin)
    : Integration(config, entities, notifications, api, configObj, plugin) {
    for (QVariantMap::const_iterator iter = config.begin(); iter != config.end(); ++iter) {
        if (iter.key() == Integration::OBJ_DATA) {
            QVariantMap map = iter.value().toMap();
            m_clientUser      = map.value("username").toString();
            m_clientPass      = map.value("password").toString();
            m_entityId        = map.value("entity_id").toString();
            m_serverIP        = map.value("server_address").toString();
            m_serverPort      = map.value("server_port").toString();
        }
    }

    m_serverURL = "http://" + m_serverIP + ":" + m_serverPort;

    m_pollingTimer = new QTimer(this);
    m_pollingTimer->setInterval(4000);
    QObject::connect(m_pollingTimer, &QTimer::timeout, this, &PlexMedia::onPollingTimerTimeout);

    // add available entity
    QStringList supportedFeatures;
    supportedFeatures << "SOURCE"
                      << "APP_NAME"
                      << "VOLUME"
                      << "VOLUME_UP"
                      << "VOLUME_DOWN"
                      << "VOLUME_SET"
                      << "MUTE"
                      << "MUTE_SET"
                      << "MEDIA_TYPE"
                      << "MEDIA_TITLE"
                      << "MEDIA_ARTIST"
                      << "MEDIA_ALBUM"
                      << "MEDIA_DURATION"
                      << "MEDIA_POSITION"
                      << "MEDIA_IMAGE"
                      << "PLAY"
                      << "PAUSE"
                      << "STOP"
                      << "PREVIOUS"
                      << "NEXT"
                      << "SEEK"
                      << "SHUFFLE"
                      << "SEARCH"
                      << "SPEAKER_CONTROL"
                      << "LIST";
    addAvailableEntity(m_entityId, "media_player", integrationId(), friendlyName(), supportedFeatures);
}

void PlexMedia::connect() {
    qCDebug(m_logCategory) << "STARTING PLEXMEDIA";
    setState(CONNECTED);

    // get auth token if we don't have it already
    if (m_authToken.isNull() || m_authToken.isEmpty()) {
        qCDebug(m_logCategory) << "Requesting auth token...";
        requestAuthToken();
    }

    //get server id if we don't have it already
    if (m_serverId.isNull() || m_serverId.isEmpty()) {
        qCDebug(m_logCategory) << "Requesting server Id...";
        getMachineIdentifier();
    }

    // start polling
    m_pollingTimer->start();
}

void PlexMedia::disconnect() {
    setState(DISCONNECTED);
    putRequest(m_playerURL + "/player/timeline/unsubscribe",""); // unsubscribe so player resets commandId counter (otherwise would be 90secs).
    m_cmdId = 0; // reset our own counter
    m_directConn = false; // reset connection to check if player still exists on reconnect.
    m_pollingTimer->stop();
}

void PlexMedia::enterStandby() { disconnect(); } //stop polling on disconnect

void PlexMedia::leaveStandby() { connect(); }

void PlexMedia::requestAuthToken() {
    QNetworkAccessManager* manager = new QNetworkAccessManager(this);
    QNetworkRequest        request;

    QObject* context = new QObject(this); // set up connection to slots on "this" object.
    QObject::connect(manager, &QNetworkAccessManager::finished, context, [=](QNetworkReply* reply) {
        if (reply->error()) {
            qCWarning(m_logCategory) << reply->errorString();
        }

        QString answer = reply->readAll();

        // convert to json
        QJsonParseError parseerror;
        QJsonDocument   doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);
        if (parseerror.error != QJsonParseError::NoError) {
            qCWarning(m_logCategory) << "JSON error : " << parseerror.errorString();
            return;
        }
        QVariantMap map = doc.toVariant().toMap();

        if (map.contains("error")) {
             qCWarning(m_logCategory) << "Error: " << map.value("error").toString();
             //display notification, likely user/pass is incorrect.
        } else {
            // store the auth token
            if (map.value("user").toMap().contains("authToken")) {
                m_authToken =  map.value("user").toMap().value("authToken").toString();
                qCDebug(m_logCategory) << "Plex user auth token: " << m_authToken;
            } else {
                qCDebug(m_logCategory) << "Cannot find authToken?";
                //other errors?
            }
        }

        reply->deleteLater();
        context->deleteLater();
        manager->deleteLater();
    });

    QObject::connect(
        manager, &QNetworkAccessManager::networkAccessibleChanged, context,
        [=](QNetworkAccessManager::NetworkAccessibility accessibility) { qCDebug(m_logCategory) << accessibility; });

    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");

    QString header_auth;
    header_auth.append(m_clientUser).append(":").append(m_clientPass);

    request.setRawHeader("Authorization", "Basic " + header_auth.toUtf8().toBase64());

    request.setRawHeader("X-Plex-Client-Identifier", m_remoteId);
    request.setRawHeader("X-Plex-Device", m_remoteSys);
    request.setRawHeader("X-Plex-Device-Name", m_remoteName);

    request.setUrl(QUrl::fromUserInput("https://plex.tv/users/sign_in.json"));

    manager->post(request, ""); // have to sign in with post
}

void PlexMedia::getMachineIdentifier() {
    QString url = m_serverURL + "/identity";

    QObject* context = new QObject(this);
    QObject::connect(this, &PlexMedia::requestReady, context, [=](const QVariantMap& map, const QString& rUrl) {
        if (rUrl == url) { 
            if (map.value("MediaContainer").toMap().contains("machineIdentifier")) {
                m_serverId = map.value("MediaContainer").toMap().value("machineIdentifier").toString();
                qCDebug(m_logCategory) << "machineIdentifier: " << m_serverId;
            } else {
                qCWarning(m_logCategory) << "machineIdentifier not found!";
                //QMap<QString, QVariant>::const_iterator i = map.constBegin();
                //while (i != map.constEnd()){
                    //qCWarning(m_logCategory) << i.key() << ": " << i.value();
                    //i++;
                //}
            }
        }
        context->deleteLater();
    });
    getRequest(url,"");
}

void PlexMedia::search(QString query) { search(query, ""); } // search all
void PlexMedia::search(QString query, QString type) {
    QString url = m_serverURL + "/search";

    query.replace(" ", "%20");

    QObject* context = new QObject(this);
    QObject::connect(this, &PlexMedia::requestReady, context, [=](const QVariantMap& map, const QString& rUrl) {
        if (rUrl == url) {  // parse the search response

            //create the response groupings
            SearchModelList* albums = new SearchModelList();
            SearchModelList* tracks = new SearchModelList();
            SearchModelList* artists = new SearchModelList();
            SearchModelList* playlists = new SearchModelList();
            SearchModelList* movies = new SearchModelList();
            SearchModelList* shows = new SearchModelList();
            SearchModelList* episodes = new SearchModelList();

            QString itemType;
            QString id;
            QString title;
            QString subtitle;
            QString image;
            QStringList commands = {"PLAY", "SHUFFLE", "QUEUE"};  // default

            QVariantList results = map.value("MediaContainer").toMap().value("Metadata").toList();
            for (int i = 0; i < results.length(); i++) {
                id = results[i].toMap().value("ratingKey").toString();

                title = results[i].toMap().value("title").toString();
                if (title.length() == 0) { title = results[i].toMap().value("titleSort").toString(); }

                itemType = results[i].toMap().value("type").toString();

                if (itemType == "album") {
                    subtitle = results[i].toMap().value("parentTitle").toString();
                    QStringList commands = {"PLAY", "SHUFFLE", "QUEUE"};
                } else if (itemType == "track") {
                    if (results[i].toMap().contains("originalTitle")) subtitle = results[i].toMap().value("originalTitle").toString();
                    else subtitle = results[i].toMap().value("grandparentTitle").toString();
                    QStringList commands = {"PLAY", "QUEUE"};
                } else if (itemType == "episode") {
                    subtitle = results[i].toMap().value("grandparentTitle").toString()
                                    + " - " + results[i].toMap().value("parentTitle").toString();
                    QStringList commands = {"PLAY", "QUEUE"};
                } else if (itemType == "playlist") {
                    subtitle = results[i].toMap().value("playlistType").toString();
                    QStringList commands = {"PLAY", "SHUFFLE"};
                } else {
                    subtitle = "";
                }
                if (results[i].toMap().contains("thumb")) {
                    image = results[i].toMap().value("thumb").toString();
                } else if (results[i].toMap().contains("grandparentThumb")) {
                    image = results[i].toMap().value("grandparentThumb").toString();
                } else {
                    image = ""; // no images for some entries
                }
                SearchModelListItem item = SearchModelListItem(id, itemType, title, subtitle, image, commands);
                if (itemType == "album") {              albums->append(item);
                } else if (itemType == "track") {       tracks->append(item);
                } else if (itemType == "artist") {      artists->append(item);
                } else if (itemType == "playlist") {    playlists->append(item);
                } else if (itemType == "movie") {       movies->append(item);
                } else if (itemType == "show") {        shows->append(item);
                } else if (itemType == "episode") {     episodes->append(item); }
            }

            //change search items based on content
            SearchModelItem* ialbums    = new SearchModelItem("albums", albums);
            SearchModelItem* itracks    = new SearchModelItem("tracks", tracks);
            SearchModelItem* iartists   = new SearchModelItem("artists", artists);
            SearchModelItem* iplaylists = new SearchModelItem("playlists", playlists);
            SearchModelItem* imovies    = new SearchModelItem("movies",movies);
            SearchModelItem* ishows     = new SearchModelItem("shows", shows);
            SearchModelItem* iepisodes  = new SearchModelItem("episodes", episodes);

            SearchModel* m_model = new SearchModel();

            m_model->append(ialbums);
            m_model->append(itracks);
            m_model->append(iartists);
            m_model->append(iplaylists);
            m_model->append(imovies);
            m_model->append(ishows);
            m_model->append(iepisodes);

            // update the entity
            EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
            if (entity) {
                MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
                me->setSearchModel(m_model);
            }
        }
        context->deleteLater();
    });

    //convert type to integer
    QString newType="";
    if (type.contains("albums")) {       newType += "9,"; } //albums and tv shows
    if (type.contains("tracks")) {       newType += "10,"; } //tracks, episodes and movies
    if (type.contains("artists")) {      newType += "8,"; }
    if (type.contains("playlists")) {    newType += "15,"; } //can only play audio playlists at the moment... maybe limit results to audio only in search?
    if (type.contains("movies")) {       newType += "1,"; }
    if (type.contains("shows")) {        newType += "2,"; }
    if (type.contains("episodes")) {     newType += "4,"; }
    if (newType.length() > 0) {          newType = newType.left(newType.length()-1); }
    else {                               newType = "1,2,4,8,9,10,15"; } //I have intentionally limited this to stuff that I've coded the controller to handle (i.e. not podcasts)

    getRequest(url, "?query=" + query + "&type=" + newType);
}

void PlexMedia::getAlbum(QString id) {
    QString url = m_serverURL + "/library/metadata/" + id + "/children";

    QString subtitle = "";
    QString type = "album";
    QString sub_type = "track";

    QObject* context = new QObject(this);
    QObject::connect(this, &PlexMedia::requestReady, context, [=](const QVariantMap& map, const QString& rUrl) {
        if (rUrl == url) {
            qCDebug(m_logCategory) << "GET ALBUM/SHOW";
            QVariantMap album = map.value("MediaContainer").toMap();
            QString id       = album.value("key").toString();
            QString title    = album.value("parentTitle").toString();
            if (album.value("viewGroup").toString() == "season") {
                QString subtitle = album.value("size").toString() + " season(s)";
                QString type     = "show";
                QString sub_type = "episode";
            } else {
                QString subtitle = album.value("grandparentTitle").toString();
                QString type     = "album";
                QString sub_type = "track";
            }
            QString image    = "";
            if (album.contains("thumb")) {
                image = album.value("thumb").toString();
            } else if (album.contains("grandparentThumb")) {
                image = album.value("grandparentThumb").toString();
            }

            QStringList commands = {"PLAY", "QUEUE"};

            BrowseModel* thisAlbum = new BrowseModel(nullptr, id, title, subtitle, type, image, commands);

            if (type == "show") {
                // as we can only go one level deep at the minute need to make a master list of all episodes. we can go to the allLeaves endpoint for this.
                QString episodes_url = m_serverURL + "/library/metadata/" + id + "/allLeaves";
                QObject* sub_context = new QObject(this);
                QObject::connect(this, &PlexMedia::requestReady, sub_context, [=](const QVariantMap& map, const QString& rUrl) {
                    if (rUrl == episodes_url) {
                        // add episodes to show
                        QVariantList episodes = map.value("MediaContainer").toMap().value("Metadata").toList();
                        for (int i = 0; i < episodes.length(); i++) {
                            thisAlbum->addItem(episodes[i].toMap().value("ratingKey").toString(),
                                           episodes[i].toMap().value("title").toString(),
                                           episodes[i].toMap().value("grandparentTitle").toString() + " - "  + episodes[i].toMap().value("parentTitle").toString(),
                                           sub_type,
                                           m_serverURL + episodes[i].toMap().value("thumb").toString(),
                                           commands);
                        }

                    }
                    sub_context->deleteLater();
                });
                getRequest(episodes_url, "");
            } else {
                // add tracks to album
                QVariantList tracks = map.value("MediaContainer").toMap().value("Metadata").toList();
                for (int i = 0; i < tracks.length(); i++) {
                    thisAlbum->addItem(tracks[i].toMap().value("ratingKey").toString(),
                                   tracks[i].toMap().value("title").toString(),
                                   tracks[i].toMap().value("grandparentTitle").toString(),
                                   sub_type,
                                   m_serverURL + tracks[i].toMap().value("parentThumb").toString(),
                                   commands);
                }
            }

            // update the entity
            updateBrowseModel(thisAlbum);
        }
        context->deleteLater();
    });
    getRequest(url, "");
}

void PlexMedia::getPlaylist(QString id) {
    QString url = m_serverURL + "/playlists/" + id + "/items";
    if (id.count("playQueues") > 0) url = m_serverURL + id; //update if we are passed a playQueue

    QObject* context = new QObject(this);

    QObject::connect(this, &PlexMedia::requestReady, context, [=](const QVariantMap& map, const QString& rUrl) {
        if (rUrl == url) {
            qCDebug(m_logCategory) << "GET PLAYLIST";
            QString title    = "";
            QString subtitle = "";
            QString type     = "playlist";
            QStringList commands = {"PLAY", "QUEUE"}; //this is albumView so commands relate to individual tracks.

            QVariantMap playlist = map.value("MediaContainer").toMap();
            if (playlist.contains("playQueueID")) { //if playqueue then
                QString id       = "/playQueues/ " + playlist.value("playQueueID").toString();
                QString title    = "Now Playing";
                QString subtitle = playlist.value("playQueueTotalCount").toString() + " item(s)";
            } else { //if standard playlist
                QString id       = playlist.value("ratingKey").toString();
                QString title    = playlist.value("title").toString();
                QString subtitle = playlist.value("leafCount").toString() + " item(s)";
            }
            //take first entry as thumb
            QString image    = m_serverURL + playlist.value("Metadata").toList()[0].toMap().value("grandparentThumb").toString();

            BrowseModel* thisPlaylist = new BrowseModel(nullptr, id, title, subtitle, type, image, commands);

            // add tracks to playlist
            QVariantList tracks = map.value("MediaContainer").toMap().value("Metadata").toList();
            for (int i = 0; i < tracks.length(); i++) {
                QString id = "";
                title = "";
                subtitle = "";

                id = tracks[i].toMap().value("ratingKey").toString();
                title = tracks[i].toMap().value("title").toString();
                subtitle = tracks[i].toMap().value("grandparentTitle").toString();

                // try and find an image. . Work backwards if we can't find anything.
                QString thumb = "";
                if (tracks[i].toMap().contains("thumb")) { QString thumb = tracks[i].toMap().value("thumb").toString();
                } else if (tracks[i].toMap().contains("parentThumb")) { QString thumb = tracks[i].toMap().value("parentThumb").toString();
                } else if (tracks[i].toMap().contains("grandparentThumb")) { QString thumb = tracks[i].toMap().value("grandparentThumb").toString(); }

                thisPlaylist->addItem(id,title,subtitle,"track",m_serverURL + thumb,commands);

                // update the entity
                updateBrowseModel(thisPlaylist);
            }
        }
        context->deleteLater();
    });
    getRequest(url, "");
}

void PlexMedia::getUserPlaylists() {
    QString all_url = m_serverURL + "/playlists";

    QObject* context = new QObject(this);
    QObject::connect(this, &PlexMedia::requestReady, context, [=](const QVariantMap& map, const QString& rUrl) {
        if (rUrl == all_url) {
            qCDebug(m_logCategory) << "GET USERS PLAYLIST";
            QString     id       = "";
            QString     title    = "";
            QString     subtitle = "";
            QString     type     = "playlist";
            QString     image    = "";
            QStringList commands = {"PLAY", "SHUFFLE"};

            BrowseModel* allPlaylists = new BrowseModel(nullptr, id, title, subtitle, type, image, commands);

            // add playlists to model
            QVariantList playlists = map.value("MediaContainer").toMap().value("Metadata").toList();
            for (int i = 0; i < playlists.length(); i++) {
               // playlists don't have an image by default. don't want to loop through HTTP calls to get thumbs so gonna suck it up. Best to include a playlist-specific default image in future.
               allPlaylists->addItem(playlists[i].toMap().value("ratingKey").toString(),
                              playlists[i].toMap().value("title").toString(),
                              playlists[i].toMap().value("leafCount").toString() + " item(s)",
                              type,
                              "",
                              commands);
            }

            // update the entity
            updateBrowseModel(allPlaylists);

            //now create a playlist of the current playQueue (if there is one)
            if (!(m_playerQueue.isNull() || m_playerQueue.isEmpty())) {
                QString now_url = m_serverURL + "/playQueues/" + m_playerQueue;

                QObject* context = new QObject(this);
                QObject::connect(this, &PlexMedia::requestReady, context, [=](const QVariantMap& map, const QString& rUrl) {
                    if (rUrl == now_url) {
                        qCDebug(m_logCategory) << "GET NOW PLAYING PLAYLIST";

                        // try and find an image. Work backwards if we can't find anything. Would be good to update this to the currently playing track?
                        QVariantList playlists = map.value("MediaContainer").toMap().value("Metadata").toList();
                        QString thumb = "";
                        if (playlists[0].toMap().contains("thumb")) { thumb = playlists[0].toMap().value("thumb").toString();
                        } else if (playlists[0].toMap().contains("parentThumb")) { thumb = playlists[0].toMap().value("parentThumb").toString();
                        } else if (playlists[0].toMap().contains("grandparentThumb")) { thumb = playlists[0].toMap().value("grandparentThumb").toString(); }

                        QStringList commands = {"PLAY", "SHUFFLE"};
                        allPlaylists->addItem("/playQueues/" + m_playerQueue,"Now Playing",map.value("MediaContainer").toMap().value("playQueueTotalCount").toString() + " item(s)",type,m_serverURL + thumb,commands);

                        // update the entity
                        updateBrowseModel(allPlaylists);
                        allPlaylists->deleteLater();
                    }
                    context->deleteLater();
                });
                getRequest(now_url, "");
            } else {
                qCDebug(m_logCategory) << "No m_playerQueue defined.";
            }
        }
        context->deleteLater();
    });
    getRequest(all_url, "");
}

void PlexMedia::getCurrentPlayer() {
    // could be upgraded as subscription based rather than polling. updates are sent to /:/timeline in XML. issue with keeping an open connection though.
    // direct polling is possible via POST {m_playerURL + "/player/timeline/poll?wait=1&commandId=" + m_cmdId} to poll but this returns XML and not JSON. Provides additional information such as volume as well.
    // apparently Win and Mac players do not respond to these poll request though? Requires a known port to be reliable hence hasve included a backup via the server.
    // implemented workflow is media info taken from session and port taken from client endpoint and then poll for details of volume and playQueue.

    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
    if (entity) { //only poll if plex is the active entity

        // if no speaker or need to get list of sources or there is no direct connection to the current/previous source.
        if (m_playerId.isNull() || m_playerId.isEmpty() || m_speakerRequest || !m_directConn || m_newTrack) {
            //qCDebug(m_logCategory) << "m_playerID.isNull =" << m_playerId.isNull()<< "m_playerID.isEmpty ="  << m_playerId.isEmpty() << "m_speakerRequest =" <<  m_speakerRequest << "m_directConn ="  << m_directConn << "m_newTrack =" << m_newTrack;
            QString url = m_serverURL + "/status/sessions"; // list of all active sessions

            if (m_pollingTimer->interval() < 4000) { m_pollingTimer->setInterval(4000); } // if we are polling the server then slow polling rate back down.

            QObject* context = new QObject(this);
            QObject::connect(this, &PlexMedia::requestReady, context, [=](const QVariantMap& map, const QString& rUrl) {
                //qCDebug(m_logCategory) << "getCurrentPlayer returned URL is: " + rUrl + ", requested URL is " + url;
                if (rUrl == url) {
                        if (map.value("MediaContainer").toMap().contains("Metadata")) {
                            m_playerConnected = true;
                            if (m_speakerRequest) getSpeakers(map); // process outstanding speaker request first.

                            QVariantList players = map.value("MediaContainer").toMap().value("Metadata").toList(); //define list of players

                            //Loop through and find the correct player.
                           int player_index = 0;
                            bool foundPlayer = false;
                            for (int i = 0; i < players.length(); i++) {
                                if (players[i].toMap().value("Player").toMap().value("machineIdentifier").toString() == m_playerId) {
                                    player_index = i;
                                    foundPlayer = true;
                                    break;
                                }
                            }
                            if (!foundPlayer) m_playerId = ""; //if player has gone offline then reset to default.

                            if (m_playerId.isNull() || m_playerId.isEmpty()) {
                                // if nothing is set then use the first player reported.
                                player_index = 0;
                                m_playerPort = "0"; //reset port. we'll try to find it next.
                            }

                            m_playerId = players[player_index].toMap().value("Player").toMap().value("machineIdentifier").toString();
                            m_playerIP = players[player_index].toMap().value("Player").toMap().value("address").toString();
                            if (m_playerPort == "0") m_playerURL = "http://" + m_playerIP + ":32500"; // if port is not set then make a guess to (potentially) enable control while we wait for /clients endpoint to confirm.
                            else m_playerURL = "http://" + m_playerIP + ":" + m_playerPort;

                            if (m_playerCurrentTrack == players[player_index].toMap().value("ratingKey").toString()) {
                                m_newTrack = false;
                            } else {
                                m_newTrack = true;
                                m_playerCurrentTrack = players[player_index].toMap().value("ratingKey").toString(); // set as current track
                            }

                            // reduce the burden if track/show/movie hasn't changed.
                            //if (m_newTrack) {
                                // get player platform
                                m_playerPlatform = players[player_index].toMap().value("Player").toMap().value("platform").toString();

                                // get the image. work backwards depending on the metadata available.
                                QString image = "";
                                if (players[player_index].toMap().contains("thumb")) { image = players[player_index].toMap().value("thumb").toString();
                                } else if (players[player_index].toMap().contains("parentThumb")) { image = players[player_index].toMap().value("parentThumb").toString();
                                } else if (players[player_index].toMap().contains("grandparentThumb")) { image = players[player_index].toMap().value("grandparentThumb").toString(); }
                                entity->updateAttrByIndex(MediaPlayerDef::MEDIAIMAGE, m_serverURL + image);

                                // get the device
                                entity->updateAttrByIndex(MediaPlayerDef::SOURCE,
                                                          players[player_index].toMap().value("Player").toMap().value("title").toString());

                                // get the track title
                                entity->updateAttrByIndex(MediaPlayerDef::MEDIATITLE,
                                                          players[player_index].toMap().value("title").toString());

                                // get the artist/show/movie parent
                                QString trackParent;
                                if (players[player_index].toMap().value("type").toString() == "track") {
                                    if (players[player_index].toMap().contains("originalTitle")) { trackParent = players[player_index].toMap().value("originalTitle").toString();
                                    } else { trackParent = players[player_index].toMap().value("grandparentTitle").toString(); } // parent is album and grandparent is artist.
                                } else if (players[player_index].toMap().value("type").toString() == "show")  { trackParent = players[player_index].toMap().value("grandparentTitle").toString() + " - " + players[player_index].toMap().value("parentTitle").toString();
                                } else if (players[player_index].toMap().value("type").toString() == "movie") { trackParent = players[player_index].toMap().value("tagLine").toString();
                                } else { trackParent = players[player_index].toMap().value("parentTitle").toString(); }

                                entity->updateAttrByIndex(MediaPlayerDef::MEDIAARTIST,
                                                          trackParent);
                            //}

                            // use opportunity to update status and progress.
                            // get the state
                            m_playerState = players[player_index].toMap().value("Player").toMap().value("state").toString();
                            if (m_playerState == "playing") {
                                entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::PLAYING);
                            } else {
                                entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::IDLE);
                            }

                            // update progress
                            entity->updateAttrByIndex(
                                MediaPlayerDef::MEDIADURATION,
                                static_cast<int>(players[player_index].toMap().value("duration").toInt() / 1000));
                            entity->updateAttrByIndex(MediaPlayerDef::MEDIAPROGRESS,
                                                      static_cast<int>(players[player_index].toMap().value("viewOffset").toInt() / 1000));

                        } else if (m_playerConnected) { // if no players then empty the player screen.
                            qCDebug(m_logCategory) << "No players discovered. Clearing player.";
                            entity->updateAttrByIndex(MediaPlayerDef::MEDIAIMAGE, "");
                            entity->updateAttrByIndex(MediaPlayerDef::SOURCE, "");
                            entity->updateAttrByIndex(MediaPlayerDef::MEDIATITLE, "");
                            entity->updateAttrByIndex(MediaPlayerDef::MEDIAARTIST, "");
                            entity->updateAttrByIndex(MediaPlayerDef::MEDIADURATION, 0);
                            entity->updateAttrByIndex(MediaPlayerDef::MEDIAPROGRESS, 0);
                            entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::OFF);
                            m_playerConnected = false;
                        }
                }
                context->deleteLater();
            });
            getRequest(url, "");
        }

        // poll if we have a player to poll
        // this is all a bit convoluted but I've tried to reduce the number of calls made to get different bits of information.
        if (!(m_playerId.isNull() || m_playerId.isEmpty())) {
            // try clients endpoint if we don't have a confirmed port yet.
            // hopefully able to grab the confirmed port straight away and then call again until we change player.
            if (m_playerPort == "0") { //only try to find port if not currently set for the active player.
                QString url = m_serverURL + "/clients";
                QObject* context = new QObject(this);
                QObject::connect(this, &PlexMedia::requestReady, context, [=](const QVariantMap& map, const QString& rUrl) {
                   if (rUrl == url) {
                        //Loop through and find the correct player.
                        QVariantList players = map.value("MediaContainer").toMap().value("Server").toList();
                        for (int i = 0; i < players.length(); i++) {
                            if (players[i].toMap().value("machineIdentifier").toString() == m_playerId) {
                                m_playerPort = players[i].toMap().value("port").toString();
                                qCDebug(m_logCategory) << "PORT FOUND, SETTING TO: " << m_playerPort;
                                break;
                            }
                        }
                    }
                    context->deleteLater();
                });
                getRequest(url, "");
                if (m_playerPort != "0") { m_playerURL = "http://" + m_playerIP + ":" + m_playerPort; }
            }

            if (!m_playerURL.isEmpty()) { //URL is emptied when player is changed.
                QString url = m_playerURL + "/player/timeline/poll";
                QString message = "?wait=1";
                getPollRequest(url, message);
            }
        }

    } //end of active entity check
}

void PlexMedia::sendCommand(const QString& type, const QString& entityId, int command, const QVariant& param) {
    if (!(type == "media_player" && entityId == m_entityId)) { return; }

    if (m_serverId.isNull() || m_serverId.isEmpty()) {
        qCWarning(m_logCategory) << "No machine identifier available.";
        getMachineIdentifier();
        return;
    }

    // check we have a player identifier so we have something to control
    if (m_playerId.isNull() || m_playerId.isEmpty()) {
        qCWarning(m_logCategory) << "No player identifier available. No players discovered.";
        return;
    }

    if (command == MediaPlayerDef::C_PLAY) {
        getRequest(m_playerURL + "/player/playback/play", "");  // normal play without browsing
    } else if (command == MediaPlayerDef::C_PLAY_ITEM || command == MediaPlayerDef::C_SHUFFLE) {
        if (param == "") {
            getRequest(m_playerURL + "/player/playback/play", ""); //nothing passed then just play?
        } else {
            QString shuffle = "0";
            if (command == MediaPlayerDef::C_SHUFFLE_PLAY) shuffle = "1";
            if (param.toMap().contains("type")) {
                if (param.toMap().value("type").toString() == "playlist") {
                    //need to use playQueues
                    QString  url     = m_serverURL + "/playQueues";
                    QString message = "?playlistID=";
                    message = message + param.toMap().value("id").toString() + "&shuffle="+ shuffle +"&continuous=0&type=audio"; //only support audio playlist at the moment
                    QObject* context = new QObject(this);
                    QObject::connect(this, &PlexMedia::requestReady, context,
                                     [=](const QVariantMap& map, const QString& rUrl) {
                                         qCDebug(m_logCategory) << "playPlaylist returned URL is: " << rUrl << ", requested URL is " << url;
                                         if (rUrl == url) {
                                             QString url = m_playerURL + "/player/playback/playMedia";
                                             QString message = "?key=/library/metadata/";
                                             message = message + param.toMap().value("id").toString() + "&offset=0&address=" + m_serverIP + "&port=" + m_serverPort + "&machineIdentifier=" + m_serverId;
                                             message = message + "&containerKey=/playQueues/" + map.value("MediaContainer").toMap().value("playQueueID").toString() + "&window=200&own=1";
                                             getRequest(url, message);
                                         }
                                         context->deleteLater();
                                     });
                    postRequest(url, message);
                } else {
                    QString  url     = m_playerURL + "/player/playback/playMedia";
                    QString message = "?key=/library/metadata/";
                    message = message + param.toMap().value("id").toString() + "&offset=0&address=" + m_serverIP + "&port=" + m_serverPort + "&machineIdentifier=" + m_serverId;
                    getRequest(url, message);
                }
            }
        }
    } else if (command == MediaPlayerDef::C_ADD_TO_QUEUE) {
        if (param.toMap().contains("type")) {
            if (param.toMap().value("type").toString() != "playlist") { // do not allow playlists to be added to the queue
                qCDebug(m_logCategory) << "ADD ITEMS(S) TO QUEUE";
                if (!(m_playerQueue.isNull() || m_playerQueue.isEmpty())) { // add to Now Playing
                    QString url     = m_serverURL + "/playQueues/" + m_playerQueue;
                    // appears to be a bug with Plex which intermittently gets fixed where this may act as "Add Next" if adding to an already defined playlist.
                    QString type_class;
                    if (param.toMap().value("type").toString() == "track" || param.toMap().value("type").toString() == "artist" || param.toMap().value("type").toString() == "album") {
                        type_class= "audio";
                    } else {
                        type_class = "video";
                    }
                    QString message = "?type=" + type_class + "&uri=server://" + m_serverId + "/com.plexapp.plugins.library/library/metadata/" + param.toMap().value("id").toString() + "&repeat=0&own=1&includeChapters=1";
                    putRequest(url, message);
                    url = m_playerURL + "/player/playback/refreshPlayQueue";
                    if (!(m_playerPlatform == "iOS")) { // currently crashes plex player in iOS! Have to rely on the natural order of things.
                        message = "?playQueueID=" + m_playerQueue;// refresh playQueue after adding to it.
                        getRequest(url, message);
                    }
                }
            }
        }
    } else if (command == MediaPlayerDef::C_PAUSE) {
        getRequest(m_playerURL + "/player/playback/pause", "");
        m_playerState = "paused";
        // if we are pausing then we are moving from a direct to indirect connection. Therefore update the button immeadiately otherwise we have to wait while the integration sorts itself out.
        EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
        if (entity) { entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::IDLE); }
    } else if (command == MediaPlayerDef::C_NEXT) {
        getRequest(m_playerURL + "/player/playback/skipNext", "");
        m_newTrack = true; // this would be picked up by the polling but better to pre-empt it and speed everything up a bit.
    } else if (command == MediaPlayerDef::C_PREVIOUS) {
        getRequest(m_playerURL + "/player/playback/skipPrevious", "");
        m_newTrack = true; // as above
    } else if (command == MediaPlayerDef::C_VOLUME_SET) {
        getRequest(m_playerURL + "/player/playback/setParameters", "?volume=" + param.toString());
    } else if (command == MediaPlayerDef::C_VOLUME_UP) {
        getRequest(m_playerURL + "/player/playback/setParameters", "?volume=" + QString::number(m_playerVol + 5)); // this should probably be standardised for API based integrations?
    } else if (command == MediaPlayerDef::C_VOLUME_DOWN) {
        getRequest(m_playerURL + "/player/playback/setParameters", "?volume=" + QString::number(m_playerVol - 5));; // this should probably be standardised for API based integrations?
    } else if (command == MediaPlayerDef::C_SEARCH) {
        search(param.toString());
    } else if (command == MediaPlayerDef::C_GETALBUM) {
        getAlbum(param.toString());
    } else if (command == MediaPlayerDef::C_GETPLAYLIST) {
        if (param.toString() == "user") {
            getUserPlaylists();
        } else {
            getPlaylist(param.toString());
        }
    } else if (command == MediaPlayerDef::C_CHANGE_SPEAKER) {
        changeSpeaker(param.toString());
    } else if (command == MediaPlayerDef::C_GET_SPEAKERS) {
        m_speakerRequest = true;
        getCurrentPlayer();
    }
}

void PlexMedia::changeSpeaker(const QString& id) {
    qCDebug(m_logCategory) << "CHANGE SPEAKER";
    m_playerId = id;
    m_playerURL = ""; // reset URL - acts a flag to not poll the old player
    m_playerPort = "0"; //reset port
    m_directConn = false; // player has changed so direct control is no longer possible.
}

void PlexMedia::getSpeakers(const QVariantMap& map) {
    qCDebug(m_logCategory) << "GET SPEAKERS";
    QString id = "";
    QString title = "";
    QString description = "";
    QString type = "speaker";
    QString image = "";
    QStringList commands = {"CONNECT"}; // default
    QStringList supported = {}; // default
    SpeakerModel* allPlayers = new SpeakerModel(nullptr, id, title, description, type, image, commands, supported);
    //Loop through all players.
    QVariantList players = map.value("MediaContainer").toMap().value("Metadata").toList();
    //qCDebug(m_logCategory) << "Number of players found: " << players.length();
    for (int i = 0; i < players.length(); i++) {
        id       = players[i].toMap().value("Player").toMap().value("machineIdentifier").toString();

        title    = players[i].toMap().value("Player").toMap().value("title").toString();
        if (players[i].toMap().value("Player").toMap().value("machineIdentifier").toString() == m_playerId) {
            title += " (Connected)";
            QStringList commands = {}; // typically cannot control remote devices
        } else {
            if (players[i].toMap().value("Player").toMap().value("local").toBool()) {
                title += " (Local)";
                QStringList commands = {"CONNECT"};
            } else {
                title += " (Remote)";
                QStringList commands = {}; // typically cannot control remote devices
            }
        }

        description = players[i].toMap().value("title").toString();
        if (description.length() == 0) { description = "Unknown"; }
        description += " (" + players[i].toMap().value("librarySectionTitle").toString() + ")";

        image    = players[i].toMap().value("User").toMap().value("thumb").toString();
        allPlayers->addItem(id, title, description, type, image, commands, supported);
    }
    // update the entity
    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
    if (entity) {
        MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
        me->setSpeakerModel(allPlayers);
    }
    m_speakerRequest = false;
}

void PlexMedia::updateEntity(const QString& entity_id, const QVariantMap& attr) {
    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(entity_id));
    if (entity) {
        // update the media player
        entity->updateAttrByIndex(MediaPlayerDef::Attributes::STATE, attr.value("state").toInt());
        entity->updateAttrByIndex(MediaPlayerDef::Attributes::SOURCE, attr.value("device").toString());
        entity->updateAttrByIndex(MediaPlayerDef::Attributes::VOLUME, attr.value("volume").toInt());
        entity->updateAttrByIndex(MediaPlayerDef::Attributes::MEDIATITLE, attr.value("title").toString());
        entity->updateAttrByIndex(MediaPlayerDef::Attributes::MEDIAARTIST, attr.value("artist").toString());
        entity->updateAttrByIndex(MediaPlayerDef::Attributes::MEDIAIMAGE, attr.value("image").toString());
    }
}

void PlexMedia::getPollRequest(const QString& url, const QString& params) {
    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
    if (entity) {
        if (m_pollingTimer->interval() > 2000) { m_pollingTimer->setInterval(2000); } // if we are actively and directly polling a client then turn up the heat!
        // create new networkacces manager and request
        QNetworkAccessManager* manager = new QNetworkAccessManager(this);
        QNetworkRequest        request;

        QObject* context = new QObject(this);
        // connect to finish signal
        QObject::connect(manager, &QNetworkAccessManager::finished, context, [=](QNetworkReply* reply) {
            int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (statusCode != 200) {
                qCWarning(m_logCategory) << "ERROR WITH POLL GET REQUEST " << statusCode << reply->readAll();
                // Note: status code of 0 indicates connection was accepted but an empty response was returned.
                qCDebug(m_logCategory) << "POLLING DID NOT RETURN VALID RESPONSE. NO DIRECT CONNECTION ASSUMED";
                m_directConn = false;
            } else {
                QString     answer = reply->readAll();
                //qCDebug(m_logCategory) << "Response from POLL GET: " << answer;

                QDomDocument doc;
                if (!doc.setContent(answer,true)) return;

                QDomNodeList timelines = doc.elementsByTagName("Timeline");
                //run through and overwrite in order - photos - video - music
                for (int i = 0; i < timelines.size(); i++) {
                    QDomElement n = timelines.item(i).toElement();
                    if (n.attribute("state") != "stopped") {
                        m_playerVol = n.attribute("volume").toInt();
                        m_playerQueue = n.attribute("playQueueID");
                        m_playerState = n.attribute("state");
                        m_playerDuration = n.attribute("duration").toInt();
                        m_playerTime = n.attribute("time").toInt();
                        if (m_playerCurrentTrack == n.attribute("ratingKey")) {
                            m_newTrack = false;
                        } else {
                            m_newTrack = true;
                        }
                        //qCDebug(m_logCategory) << "State is: " << m_playerState << ", Progress is: " << static_cast<int>(m_playerTime/1000) << " of " << static_cast<int>(m_playerDuration/1000);
                    }
                }

                entity->updateAttrByIndex(MediaPlayerDef::VOLUME, m_playerVol);

                // get the state
                if (m_playerState == "playing") {
                    entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::PLAYING);
                } else {
                    entity->updateAttrByIndex(MediaPlayerDef::STATE, MediaPlayerDef::IDLE);
                }

                // update progress
                entity->updateAttrByIndex(MediaPlayerDef::MEDIADURATION, static_cast<int>(m_playerDuration / 1000));
                entity->updateAttrByIndex(MediaPlayerDef::MEDIAPROGRESS, static_cast<int>(m_playerTime / 1000));
                m_directConn = true;
            }

            reply->deleteLater();
            context->deleteLater();
            manager->deleteLater();
        });

        QObject::connect(
            manager, &QNetworkAccessManager::networkAccessibleChanged, context,
            [=](QNetworkAccessManager::NetworkAccessibility accessibility) { qCDebug(m_logCategory) << accessibility; });

        // set headers
        //request.setRawHeader("Accept", "application/json"); //only responds in XML.
        request.setRawHeader("X-Plex-Token", m_authToken.toLocal8Bit());
        request.setRawHeader("X-Plex-Client-Identifier", m_remoteId);
        request.setRawHeader("X-Plex-Device", m_remoteSys);
        request.setRawHeader("X-Plex-Device-Name", m_remoteName);
        request.setRawHeader("X-Plex-Provides", "controller");
        request.setRawHeader("X-Plex-Target-Client-Identifier", m_playerId.toLocal8Bit());

        // set the URL
        if (params.length() > 0) request.setUrl(QUrl::fromUserInput(url + params + "&commandId=" +  QString::number(m_cmdId)));
        else request.setUrl(QUrl::fromUserInput(url + "?commandId=" +  QString::number(m_cmdId)));

        //qCDebug(m_logCategory) << "Sending as POLL GET: " << request.url().toString();

        // send the get request
        manager->get(request);
        m_cmdId++;
    }
}

void PlexMedia::getRequest(const QString& url, const QString& params) {
    if (m_authToken.isNull() || m_authToken.isEmpty()) {
        qCWarning(m_logCategory) << "No access token available.";
        requestAuthToken();
        return;
    }

    // create new networkacces manager and request
    QNetworkAccessManager* manager = new QNetworkAccessManager(this);
    QNetworkRequest        request;

    QObject* context = new QObject(this);

    // connect to finish signal
    QObject::connect(manager, &QNetworkAccessManager::finished, context, [=](QNetworkReply* reply) {
        if (reply->error()) {
            qCWarning(m_logCategory) << reply->errorString();
        }

        QString     answer = reply->readAll();
        //qCDebug(m_logCategory) << "Response from GET: " << answer;

        if (answer != "") {
            QVariantMap map;
            // convert to json
            QJsonParseError parseerror;
            QJsonDocument   doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);
            if (parseerror.error != QJsonParseError::NoError) {
                qCWarning(m_logCategory) << "JSON error : " << parseerror.errorString();
                return;
            }

            // createa a map object
            map = doc.toVariant().toMap();
            emit requestReady(map, url);
        }

        reply->deleteLater();
        context->deleteLater();
        manager->deleteLater();
    });

    QObject::connect(
        manager, &QNetworkAccessManager::networkAccessibleChanged, context,
        [=](QNetworkAccessManager::NetworkAccessibility accessibility) { qCDebug(m_logCategory) << accessibility; });

    // set headers
    request.setRawHeader("Accept", "application/json"); //need this to get a json rather than xml response from the server.
    request.setRawHeader("X-Plex-Token", m_authToken.toLocal8Bit());
    request.setRawHeader("X-Plex-Client-Identifier", m_remoteId);
    request.setRawHeader("X-Plex-Device", m_remoteSys);
    request.setRawHeader("X-Plex-Device-Name", m_remoteName);
    request.setRawHeader("X-Plex-Provides", "controller");
    request.setRawHeader("X-Plex-Target-Client-Identifier", m_playerId.toLocal8Bit());

    // set the URL
    if (params.length() > 0) { request.setUrl(QUrl::fromUserInput(url + params + "&commandId=" +  QString::number(m_cmdId)));
    } else { request.setUrl(QUrl::fromUserInput(url + "?commandId=" +  QString::number(m_cmdId))); }

    qCDebug(m_logCategory) << "Sending as GET: " + request.url().toString();

    // send the get request
    manager->get(request);
    m_cmdId++;
}

void PlexMedia::postRequest(const QString& url, const QString& params) {
    if (m_authToken.isNull() || m_authToken.isEmpty()) {
        qCWarning(m_logCategory) << "No access token available";
        requestAuthToken();
        return;
    }

    // create new networkacces manager and request
    QNetworkAccessManager* manager = new QNetworkAccessManager(this);
    QNetworkRequest        request;

    QObject* context = new QObject(this);

    // connect to finish signal
    QObject::connect(manager, &QNetworkAccessManager::finished, context, [=](QNetworkReply* reply) {
        int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode != 200) {
            qCWarning(m_logCategory) << "ERROR WITH POST REQUEST " << statusCode << reply->readAll();
        } else {
            QString     answer = reply->readAll();
            //qCDebug(m_logCategory) << "Response from POST: " << answer;

            if (answer != "") {
                QVariantMap map;
                // convert to json
                QJsonParseError parseerror;
                QJsonDocument   doc = QJsonDocument::fromJson(answer.toUtf8(), &parseerror);
                if (parseerror.error != QJsonParseError::NoError) {
                    qCWarning(m_logCategory) << "JSON error : " << parseerror.errorString();
                    return;
                }
                // createa a map object
                map = doc.toVariant().toMap();
                emit requestReady(map, url);
            }

        }

        reply->deleteLater();
        context->deleteLater();
        manager->deleteLater();
    });

    QObject::connect(
        manager, &QNetworkAccessManager::networkAccessibleChanged, context,
        [=](QNetworkAccessManager::NetworkAccessibility accessibility) { qCDebug(m_logCategory) << accessibility; });

    // set headers
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-Plex-Token", m_authToken.toLocal8Bit());
    request.setRawHeader("X-Plex-Client-Identifier", m_remoteId);
    request.setRawHeader("X-Plex-Device", m_remoteSys);
    request.setRawHeader("X-Plex-Device-Name", m_remoteName);
    request.setRawHeader("X-Plex-Provides", "controller");
    request.setRawHeader("X-Plex-Target-Client-Identifier", m_playerId.toLocal8Bit());

    // set the URL
    if (params.length() > 0) { request.setUrl(QUrl::fromUserInput(url + params + "&commandId=" +  QString::number(m_cmdId)));
    } else { request.setUrl(QUrl::fromUserInput(url + "?commandId=" +  QString::number(m_cmdId))); }

    qCDebug(m_logCategory) << "Sending as POST: " << request.url().toString();

    // send the get request
    manager->post(request, "");
    m_cmdId++;
}

void PlexMedia::putRequest(const QString& url, const QString& params) {
    if (m_authToken.isNull() || m_authToken.isEmpty()) {
        qCWarning(m_logCategory) << "No access token available";
        requestAuthToken();
        return;
    }

    // create new networkacces manager and request
    QNetworkAccessManager* manager = new QNetworkAccessManager(this);
    QNetworkRequest        request;

    QObject* context = new QObject(this);

    // connect to finish signal
    QObject::connect(manager, &QNetworkAccessManager::finished, context, [=](QNetworkReply* reply) {
        int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (statusCode != 200) {
            qCWarning(m_logCategory) << "ERROR WITH PUT REQUEST " << statusCode << reply->readAll();
        }
        reply->deleteLater();
        context->deleteLater();
        manager->deleteLater();
    });

    QObject::connect(
        manager, &QNetworkAccessManager::networkAccessibleChanged, context,
        [=](QNetworkAccessManager::NetworkAccessibility accessibility) { qCDebug(m_logCategory) << accessibility; });

    // set headers
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("X-Plex-Token", m_authToken.toLocal8Bit());
    request.setRawHeader("X-Plex-Client-Identifier", m_remoteId);
    request.setRawHeader("X-Plex-Device", m_remoteSys);
    request.setRawHeader("X-Plex-Device-Name", m_remoteName);
    request.setRawHeader("X-Plex-Provides", "controller");
    request.setRawHeader("X-Plex-Target-Client-Identifier", m_playerId.toLocal8Bit());

    // set the URL
    if (params.length() > 0) { request.setUrl(QUrl::fromUserInput(url + params + "&commandId=" +  QString::number(m_cmdId)));
    } else { request.setUrl(QUrl::fromUserInput(url + "?commandId=" +  QString::number(m_cmdId))); }

    qCDebug(m_logCategory) << "Sending as PUT: " << request.url().toString();

    // send the put request
    manager->put(request, "");
    m_cmdId++;
}

void PlexMedia::onPollingTimerTimeout() { getCurrentPlayer(); }


void PlexMedia::updateBrowseModel(BrowseModel * model) {
    // update the entity
    EntityInterface* entity = static_cast<EntityInterface*>(m_entities->getEntityInterface(m_entityId));
    if (entity) {
        MediaPlayerInterface* me = static_cast<MediaPlayerInterface*>(entity->getSpecificInterface());
        me->setBrowseModel(model);
    }
}
