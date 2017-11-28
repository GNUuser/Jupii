/* Copyright (C) 2017 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "utils.h"

#include <QNetworkInterface>
#include <QNetworkAddressEntry>
#include <QHostAddress>
#include <QList>
#include <QAbstractSocket>
#include <QDebug>
#include <QStringList>
#include <QFile>
#include <QIODevice>
#include <QTextStream>
#include <QDir>

#include "settings.h"

Utils* Utils::m_instance = nullptr;

Utils::Utils(QObject *parent) : QObject(parent)
{
    createCacheDir();
}

Utils* Utils::instance(QObject *parent)
{
    if (Utils::m_instance == nullptr) {
        Utils::m_instance = new Utils(parent);
    }

    return Utils::m_instance;
}

QString Utils::friendlyDeviceType(const QString &deviceType)
{
    auto list = deviceType.split(':');
    return list.mid(list.length()-2).join(':');
    //return list.at(list.length()-2);
}

QString Utils::friendlyServiceType(const QString &serviceType)
{
    auto list = serviceType.split(':');
    return list.mid(list.length()-2).join(':');
    //return list.at(list.length()-2);
}

QString Utils::secToStr(int value)
{
    int s = value % 60;
    int m = (value - s)/60 % 3600;
    int h = (value - s - m * 60)/3600;

    QString str;
    QTextStream sstr(&str);

    if (h > 0)
        sstr << h << ":";

    sstr << m << ":";

    if (s < 10)
        sstr << "0";

    sstr << s;

    return str;
}

bool Utils::checkNetworkIf()
{
    QString ifname, address;
    return getNetworkIf(ifname, address);
}

bool Utils::getNetworkIf(QString& ifname, QString& address)
{
    auto if_list = QNetworkInterface::allInterfaces();

    for (auto interface : if_list) {

        if (interface.isValid() &&
            !interface.flags().testFlag(QNetworkInterface::IsLoopBack) &&
            interface.flags().testFlag(QNetworkInterface::IsUp) &&
            interface.flags().testFlag(QNetworkInterface::IsRunning) &&
            (interface.name() == "eth2" ||
             interface.name() == "wlan0" ||
             interface.name() == "tether")) {

            //qDebug() << "interface:" << interface.name();

            ifname = interface.name();

            auto addra = interface.addressEntries();
            for (auto a : addra) {
                auto ha = a.ip();
                if (ha.protocol() == QAbstractSocket::IPv4Protocol ||
                    ha.protocol() == QAbstractSocket::IPv6Protocol) {
                    //qDebug() << " address:" << ha.toString();
                    address = ha.toString();
                    return true;
                }
            }
        }
    }

    return false;
}

bool Utils::writeToFile(const QString &filename, const QByteArray &data)
{
    const QString cacheDir = Settings::instance()->getCacheDir();

    QFile f(cacheDir + "/" + filename);
    if (!f.exists()) {
        if (f.open(QIODevice::WriteOnly)) {
            f.write(data);
            f.close();
        } else {
            qWarning() << "Can't open file" << f.fileName() <<
                          "for writing (" + f.errorString() + ")";
            return false;
        }
    } else {
        qDebug() << "File" << f.fileName() << "already exists";
    }

    return true;
}

bool Utils::createCacheDir()
{
    const QString cacheDir = Settings::instance()->getCacheDir();

    if (!QFile::exists(cacheDir)) {
        if (!QDir::root().mkpath(cacheDir)) {
            qWarning() << "Unable to create cache dir!";
            return false;
        }
    }

    return true;
}

bool Utils::readFromFile(const QString &filename, QByteArray &data)
{
    const QString cacheDir = Settings::instance()->getCacheDir();

    QFile f(cacheDir + "/" + filename);
    if (f.exists()) {
        if (f.open(QIODevice::ReadOnly)) {
            data = f.readAll();
            f.close();
            return true;
        } else {
            qWarning() << "Can't open file" << f.fileName() <<
                          "for reading (" + f.errorString() + ")";
        }
    } else {
        qDebug() << "File" << f.fileName() << "doesn't exist";
    }

    return false;
}

QString Utils::hash(const QString &value)
{
    return QString::number(qHash(value));
}
