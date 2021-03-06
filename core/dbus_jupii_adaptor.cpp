/*
 * This file was generated by qdbusxml2cpp version 0.8
 * Command line was: qdbusxml2cpp /home/mersdk/share/dev/Jupii/sailfish/../dbus/org.jupii.xml -a ../core/dbus_jupii_adaptor
 *
 * qdbusxml2cpp is Copyright (C) 2016 The Qt Company Ltd.
 *
 * This is an auto-generated file.
 * Do not edit! All changes made to it will be lost.
 */

#include "../core/dbus_jupii_adaptor.h"
#include <QtCore/QMetaObject>
#include <QtCore/QByteArray>
#include <QtCore/QList>
#include <QtCore/QMap>
#include <QtCore/QString>
#include <QtCore/QStringList>
#include <QtCore/QVariant>

/*
 * Implementation of adaptor class PlayerAdaptor
 */

PlayerAdaptor::PlayerAdaptor(QObject *parent)
    : QDBusAbstractAdaptor(parent)
{
    // constructor
    setAutoRelaySignals(true);
}

PlayerAdaptor::~PlayerAdaptor()
{
    // destructor
}

bool PlayerAdaptor::canControl() const
{
    // get the value of property canControl
    return qvariant_cast< bool >(parent()->property("canControl"));
}

void PlayerAdaptor::addPath(const QString &path, const QString &name)
{
    // handle method call org.jupii.Player.addPath
    QMetaObject::invokeMethod(parent(), "addPath", Q_ARG(QString, path), Q_ARG(QString, name));
}

void PlayerAdaptor::addUrl(const QString &url, const QString &name)
{
    // handle method call org.jupii.Player.addUrl
    QMetaObject::invokeMethod(parent(), "addUrl", Q_ARG(QString, url), Q_ARG(QString, name));
}

void PlayerAdaptor::appendPath(const QString &path)
{
    // handle method call org.jupii.Player.appendPath
    QMetaObject::invokeMethod(parent(), "appendPath", Q_ARG(QString, path));
}

void PlayerAdaptor::clearPlaylist()
{
    // handle method call org.jupii.Player.clearPlaylist
    QMetaObject::invokeMethod(parent(), "clearPlaylist");
}

