/* Copyright (C) 2017 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#ifndef AVTRANSPORT_H
#define AVTRANSPORT_H

#include <QString>
#include <QTimer>
#include <QMutex>

#include <libupnpp/control/avtransport.hxx>
#include <libupnpp/control/service.hxx>
#include "libupnpp/control/cdircontent.hxx"

#include "service.h"

class AVTransport : public Service
{
    Q_OBJECT
    Q_PROPERTY (QString currentURI READ getCurrentURI NOTIFY uriChanged)
    Q_PROPERTY (QString currentPath READ getCurrentPath NOTIFY uriChanged)
    Q_PROPERTY (QString nextURI READ getNextURI NOTIFY uriChanged)
    Q_PROPERTY (QString nextPath READ getNextPath NOTIFY uriChanged)

    Q_PROPERTY (int transportState READ getTransportState NOTIFY transportStateChanged)
    Q_PROPERTY (int transportStatus READ getTransportStatus NOTIFY transportStatusChanged)
    Q_PROPERTY (int numberOfTracks READ getNumberOfTracks NOTIFY numberOfTracksChanged)
    Q_PROPERTY (int currentTrack READ getCurrentTrack NOTIFY currentTrackChanged)
    Q_PROPERTY (int currentTrackDuration READ getCurrentTrackDuration NOTIFY currentTrackDurationChanged)
    Q_PROPERTY (int relativeTimePosition READ getRelativeTimePosition NOTIFY relativeTimePositionChanged)
    Q_PROPERTY (int absoluteTimePosition READ getAbsoluteTimePosition NOTIFY absoluteTimePositionChanged)
    Q_PROPERTY (int speed READ getSpeed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY (QString currentClass READ getCurrentClass NOTIFY currentMetaDataChanged)
    Q_PROPERTY (QString currentTitle READ getCurrentTitle NOTIFY currentMetaDataChanged)
    Q_PROPERTY (QString currentAuthor READ getCurrentAuthor NOTIFY currentMetaDataChanged)
    Q_PROPERTY (QString currentDescription READ getCurrentDescription NOTIFY currentMetaDataChanged)
    Q_PROPERTY (QString currentAlbumArtURI READ getCurrentAlbumArtURI NOTIFY currentAlbumArtChanged)
    Q_PROPERTY (QString currentAlbum READ getCurrentAlbum NOTIFY currentMetaDataChanged)

    Q_PROPERTY (bool nextSupported READ getNextSupported NOTIFY transportActionsChanged)
    Q_PROPERTY (bool pauseSupported READ getPauseSupported NOTIFY transportActionsChanged)
    Q_PROPERTY (bool playSupported READ getPlaySupported NOTIFY transportActionsChanged)
    Q_PROPERTY (bool previousSupported READ getPreviousSupported NOTIFY transportActionsChanged)
    Q_PROPERTY (bool seekSupported READ getSeekSupported NOTIFY transportActionsChanged)
    Q_PROPERTY (bool stopSupported READ getStopSupported NOTIFY transportActionsChanged)

public:
    enum TransportState {Unknown, Stopped, Playing, Transitioning,
                         PausedPlayback, PausedRecording, Recording,
                         NoMediaPresent
                        };
    Q_ENUM(TransportState)

    enum TransportStatus {TPS_Unknown, TPS_Ok, TPS_Error};
    Q_ENUM(TransportStatus)

    explicit AVTransport(QObject *parent = nullptr);

    Q_INVOKABLE void play();
    Q_INVOKABLE void stop();
    Q_INVOKABLE void pause();
    Q_INVOKABLE void next();
    Q_INVOKABLE void previous();
    Q_INVOKABLE void seek(int value);
    Q_INVOKABLE void setLocalContent(const QString &cpath, const QString &npath);
    Q_INVOKABLE void setCurrentLocalContent(const QString &path);
    Q_INVOKABLE void setNextLocalContent(const QString &path);
    Q_INVOKABLE void clearNextUri();
    Q_INVOKABLE void asyncUpdate(int initDelay = 0, int postDelay = 500);

    int getTransportState();
    int getTransportStatus();
    int getNumberOfTracks();
    int getCurrentTrack();
    int getCurrentTrackDuration();
    int getRelativeTimePosition();
    int getAbsoluteTimePosition();
    int getSpeed();
    QString getCurrentURI();
    QString getNextURI();
    QString getCurrentPath();
    QString getNextPath();
    QString getCurrentClass();
    QString getCurrentAuthor();
    QString getCurrentTitle();
    QString getCurrentDescription();
    QString getCurrentAlbum();
    QString getCurrentAlbumArtURI();
    bool getNextSupported();
    bool getPauseSupported();
    bool getPlaySupported();
    bool getPreviousSupported();
    bool getSeekSupported();
    bool getStopSupported();

    void setSpeed(int value);

signals:
    void transportStateChanged();
    void transportStatusChanged();
    void numberOfTracksChanged();
    void currentTrackChanged();
    void currentTrackDurationChanged();
    void relativeTimePositionChanged();
    void absoluteTimePositionChanged();
    void currentMetaDataChanged();
    void currentAlbumArtChanged();
    void uriChanged();
    void speedChanged();
    void transportActionsChanged();
    void updated();

private slots:
    void timerEvent();
    void transportStateHandler();
    void trackChangedHandler();
    void seekTimeout();
    void handleApplicationStateChanged(Qt::ApplicationState state);

private:
    int m_transportState = Unknown;
    int m_transportStatus = TPS_Unknown;
    int m_numberOfTracks = 0;
    int m_currentTrack = 0;
    int m_currentTrackDuration = 0;
    int m_relativeTimePosition = 0;
    int m_absoluteTimePosition = 0;
    int m_speed = 1;
    int m_currentTransportActions = 0;
    int m_oldTimePosition = 0;
    QString m_id = "";
    QString m_currentClass = "";
    QString m_currentTitle = "";
    QString m_currentAuthor = "";
    QString m_currentDescription = "";
    QString m_currentAlbumArtURI = "";
    QString m_currentAlbum = "";

    QString m_currentURI = "";
    QString m_nextURI = "";
    bool m_emitUriChanged = false;

    QTimer m_seekTimer;
    int m_futureSeek = 0;

    QMutex m_updateMutex;

    void changed(const QString &name, const QVariant &value);
    UPnPClient::Service* createUpnpService(const UPnPClient::UPnPDeviceDesc &ddesc,
                                           const UPnPClient::UPnPServiceDesc &sdesc);
    void postInit();
    void postDeInit();

    UPnPClient::AVTransport* s();
    void fakeUpdateRelativeTimePosition();
    void updateTransportInfo();
    void updatePositionInfo();
    void updateMediaInfo();
    void updateCurrentTransportActions();
    void update(int initDelay = 500, int postDelay = 500);
    void asyncUpdateTransportInfo();
    void asyncUpdatePositionInfo();
    void asyncUpdateMediaInfo();
    void asyncUpdateCurrentTransportActions();
    void updateTrackMeta(const UPnPClient::UPnPDirObject &trackmeta);
    void needTimerCheck();
    std::string type() const;
};

#endif // AVTRANSPORT_H