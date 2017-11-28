/* Copyright (C) 2017 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <QDebug>
#include <QByteArray>
#include <QStandardPaths>
#include <QTime>

#include "settings.h"
#include "directory.h"
#include "utils.h"

#include <libupnpp/control/description.hxx>

Settings* Settings::inst = nullptr;

Settings::Settings(QObject *parent) :
    QObject(parent),
    TaskExecutor(parent, 1),
    settings(parent)
{
    // Seed init, needed for key generation
    qsrand(QTime::currentTime().msec());
}

Settings* Settings::instance()
{
    if (Settings::inst == nullptr) {
        Settings::inst = new Settings();
    }

    return Settings::inst;
}

void Settings::setPort(int value)
{
    if (getPort() != value) {
        settings.setValue("port", value);
        emit portChanged();
    }
}

int Settings::getPort()
{
    return settings.value("port", 9092).toInt();
}

void Settings::setForwardTime(int value)
{
    if (value < 1 || value > 60)
        return; // incorrect value

    if (getForwardTime() != value) {
        settings.setValue("forwardtime", value);
        emit forwardTimeChanged();
    }
}

int Settings::getForwardTime()
{
    // Default value is 10 s
    return settings.value("forwardtime", 10).toInt();
}

void Settings::setFavDevices(const QHash<QString,QVariant> &devs)
{
    settings.setValue("favdevices", devs);
    emit favDevicesChanged();
}

QString Settings::getCacheDir()
{
   return QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

void Settings::asyncAddFavDevice(const QString &id)
{
    startTask([this, &id](){
        addFavDevice(id);
    });
}

void Settings::asyncRemoveFavDevice(const QString &id)
{
    startTask([this, &id](){
        removeFavDevice(id);
    });
}

void Settings::addFavDevice(const QString &id)
{
    auto list = getFavDevices();
    if (list.contains(id))
        return;

    QString url;
    if (writeDeviceXML(id, url)) {
        list.insert(id, url);
        setFavDevices(list);
    }
}

void Settings::removeFavDevice(const QString &id)
{
    auto list = getFavDevices();
    if (list.contains(id)) {
        list.remove(id);
        setFavDevices(list);
    }
}

bool Settings::writeDeviceXML(const QString &id, QString& url)
{
    UPnPClient::UPnPDeviceDesc ddesc;

    if (Directory::instance()->getDeviceDesc(id, ddesc)) {

        QString _id(id);
        QString filename = "device-" + _id.replace(':','-') + ".xml";

        auto u = Utils::instance();
        if (u->writeToFile(filename, QByteArray::fromStdString(ddesc.XMLText))) {
            url = QString::fromStdString(ddesc.URLBase);
            return true;
        } else {
            qWarning() << "Can't write description file for device" << id;
        }

    } else {
        qWarning() << "Can't find device description for" << id;
    }

    return false;
}

bool Settings::readDeviceXML(const QString &id, QByteArray& xml)
{
    QString _id(id);
    QString filename = "device-" + _id.replace(':','-') + ".xml";

    auto u = Utils::instance();
    if (u->readFromFile(filename, xml)) {
        return true;
    } else {
        qWarning() << "Can't read description file for device" << id;
    }

    return false;
}

QHash<QString,QVariant> Settings::getFavDevices()
{
    return settings.value("favdevices",QHash<QString,QVariant>()).toHash();
}

void Settings::setLastDir(const QString& value)
{
    if (getLastDir() != value) {
        settings.setValue("lastdir", value);
        emit lastDirChanged();
    }
}

QString Settings::getLastDir()
{
    return settings.value("lastdir", "").toString();
}

void Settings::setShowAllDevices(bool value)
{
    if (getShowAllDevices() != value) {
        settings.setValue("showalldevices", value);
        emit showAllDevicesChanged();
    }
}

bool Settings::getShowAllDevices()
{
    return settings.value("showalldevices", false).toBool();
}

void Settings::setImageSupported(bool value)
{
    if (getImageSupported() != value) {
        settings.setValue("imagesupported", value);
        emit imageSupportedChanged();
    }
}

bool Settings::getImageSupported()
{
    return settings.value("imagesupported", false).toBool();
}

QByteArray Settings::resetKey()
{
    QByteArray key;

    // key is 10 random chars
    for (uint i = 0; i < 10; ++i)
        key.append(static_cast<char>(qrand()));

    settings.setValue("key", key);

    return key;
}

QByteArray Settings::getKey()
{
    QByteArray key = settings.value("key").toByteArray();

    if (key.isEmpty()) {
        key = resetKey();
    }

    return key;
}
