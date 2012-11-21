/*
 * Copyright (c) 2012 Damien Grauser (Damien.Grauser@pev-geneve.ch)
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#include "JouleDevice.h"
#include "DownloadRideDialog.h"
#include "PowerTapUtil.h"
#include "Device.h"
#include <math.h>
#include <errno.h>
//#include <termios.h>

#define JOULE_DEBUG false // debug traces

// Start pattern
#define START_1            0x10
#define START_2            0x02

// Commands send from PC to Joule
#define READ_UNIT_VERSION  0x2000
#define GET_FREE_SPACE     0x2002
#define READ_SYSTEM_INFO   0x2003
#define READ_RIDE_SUMMARY  0x2020
#define READ_RIDE_DETAIL   0x2021
#define PAGE_RIDE_DETAIL   0x2022
#define ERASE_RIDE_DETAIL  0x2024

static bool jouleRegistered =
    Devices::addType("Joule GPS (BETA)", DevicesPtr(new JouleDevices()) );

QString
JouleDevices::downloadInstructions() const
{
    return ("Make sure the Joule GPS unit is turned ON\n");
}

DevicePtr
JouleDevices::newDevice( CommPortPtr dev, Device::StatusCallback cb )
{
    return DevicePtr( new JouleDevice( dev, cb ));
}

static QString
cEscape(char *buf, int len)
{
    char *result = new char[4 * len + 1];
    char *tmp = result;
    for (int i = 0; i < len; ++i) {
        if (buf[i] == '"')
            tmp += sprintf(tmp, "\\\"");
        else if (isprint(buf[i]))
            *(tmp++) = buf[i];
        else
            tmp += sprintf(tmp, "\\x%02x", 0xff & (unsigned) buf[i]);
    }
    return result;
}

static int
bcd2Int(char c)
{
    return (0xff & c) - (((0xff & c)/16)*6);
}

static int
qByteArray2Int(QByteArray array)
{
    int result = 0;
    for (int i = 0; i < array.size(); i++)
    {
        result += pow(256,i)*(0xff & array.at(i));
    }
    return result;
}

static int
readOneByOne(CommPortPtr dev, void *buf, size_t nbyte, QString &err)
{
    char * data = ((char *)buf);
    int rtn=0;

    for (int i = 0; i < nbyte; i++)
    {
        int n = dev->read(data + i, 1, err);
        if (n <= 0) {
            return rtn;
        }
        if (data[i] == START_1){
            int n = dev->read(data + i, 1, err);
            if (n <= 0) {
                return rtn;
            }
        }
        rtn++;
    }
    return rtn;
}

bool
JouleDevice::download( const QDir &tmpdir,
                         QList<DeviceDownloadFile> &files,
                         CancelCallback cancelCallback,
                         ProgressCallback progressCallback,
                         QString &err)
{
    if (JOULE_DEBUG) printf("download Joule GPS");

    if (!dev->open(err)) {
        err = "ERROR: open failed: " + err;
        return false;
    }

    dev->setBaudRate(57600, err);

    QString version = QString("");
    QString serial = QString("");
    QByteArray versionArray;
    QByteArray systemInfoArray;

    getUnitVersion(version, versionArray, err);
    statusCallback("Version"+version);

    getSystemInfo(serial, systemInfoArray, err);

    QList<DeviceStoredRideItem> trainings;
    if (!getDownloadableRides(trainings, err))
        return false;

    for (int i=0; i<trainings.count(); i++) {
        statusCallback(QString("Read ride detail for ride %1/%2").arg(i+1).arg(trainings.count()));
        JoulePacket request(READ_RIDE_DETAIL);
        int id1 = (trainings.at(i).id>255?trainings.at(i).id-255:trainings.at(i).id);
        int id2 = (trainings.at(i).id>255?trainings.at(i).id%255:0);
        request.addToPayload((char)id1);
        request.addToPayload((char)id2);
        request.addToPayload((uint16_t)0xFFFF); // Total Ride#
        request.addToPayload((uint16_t)0x0000); // Start Page#

        if (!request.write(dev, err)) return false;

        if (cancelCallback())
        {
            err = "download cancelled";
            return false;
        }

        JoulePacket response = JoulePacket(PAGE_RIDE_DETAIL);
        if (response.read(dev, err)) {

            if (response.payload.size() < 4096)
            {
                err = "no data";
                return false;
            }

            int page = qByteArray2Int(response.payload.left(2));
            int data_version = qByteArray2Int(response.payload.mid(2,1));
            int firmware = qByteArray2Int(response.payload.mid(3,1));

            QString data = QString("%1 %2-%3").arg(page).arg(data_version).arg(firmware);
            qDebug() << data;

            if (page < 65535) {
                // create temporary file
                QString tmpl = tmpdir.absoluteFilePath(".joule.XXXXXX"); //".joulegps.XXXXXX"
                QTemporaryFile tmp(tmpl);
                tmp.setAutoRemove(false);

                if (!tmp.open()) {
                    err = "Failed to create temporary file "
                        + tmpl + ": " + tmp.error();
                    return false;
                }

                // timestamp from the first training
                struct tm start;
                start.tm_sec = bcd2Int(response.payload.at(4));
                start.tm_min = bcd2Int(response.payload.at(5));
                start.tm_hour = bcd2Int(response.payload.at(6));
                start.tm_mday = bcd2Int(response.payload.at(7));
                start.tm_mon = bcd2Int(response.payload.at(8))-1;
                start.tm_year = 2000 + bcd2Int(response.payload.at(9))-1900;
                start.tm_isdst = -1;

                DeviceDownloadFile file;
                file.extension = "bin2";
                file.name = tmp.fileName();
                file.startTime.setTime_t( mktime( &start ));
                files.append(file);

                QTextStream os(&tmp);
                os << hex;

                qDebug() << tmp.fileName() << "-" << tmpl;
                tmp.write(versionArray);
                tmp.write(systemInfoArray);
                tmp.write(response.dataArray());

                int _try = 1;
                while (page < 65535) {
                    qDebug() << "page"<<page;
                    if (JOULE_DEBUG) printf("Acknowledge\n");

                    request = JoulePacket(PAGE_RIDE_DETAIL);
                    request.addToPayload(response.payload.left(2).data(),2);

                    if (!request.write(dev, err))
                        return false;

                    response = JoulePacket(PAGE_RIDE_DETAIL);

                    bool success = false;
                    while (!success) {
                        if (response.read(dev, err)) {
                            page = qByteArray2Int(response.payload.left(2));

                            tmp.write(response.dataArray());
                            success = true;
                        } else {
                            if (_try == 3)
                                return false;
                            else {
                                JoulePacket request(READ_RIDE_DETAIL);
                                //request.addToPayload((uint16_t)0x0200); // Ride#
                                request.addToPayload((char)i);
                                request.addToPayload((char)0x00);
                                request.addToPayload((uint16_t)0xFFFF); // Total Ride#
                                request.addToPayload((uint16_t)page); // Start Page#

                                if (!request.write(dev, err)) return false;

                                response = JoulePacket(PAGE_RIDE_DETAIL);
                                _try ++;
                            }
                        }
                    }
                }
                tmp.close();
            }
            qDebug() << "end page" << page;

            if (JOULE_DEBUG) printf("Acknowledge\n");
            request = JoulePacket(PAGE_RIDE_DETAIL);
            request.addToPayload(response.payload.left(2).data(),2);

            if (!request.write(dev, err)) return false;


        } else
            i=99;
    }

    dev->close();

    return true;
}

bool
JouleDevice::getUnitVersion(QString &version, QByteArray &array, QString &err)
{
    statusCallback("Get Unit Software Version...");
    if (JOULE_DEBUG) printf("Get Unit Software Version\n");

    JoulePacket request(READ_UNIT_VERSION);

    if (!request.write(dev, err)) return false;

    JoulePacket response = JoulePacket(READ_UNIT_VERSION);
    if (response.read(dev, err)) {

        if (response.payload.length()>4) {
            array = response.dataArray();
            int major_version = qByteArray2Int(response.payload.left(1));
            int minor_version = qByteArray2Int(response.payload.mid(1,2));
            int data_version = qByteArray2Int(response.payload.right(2));

            version = QString(minor_version<100?"%1.0%2 (%3)":"%1.%2 (%3)").arg(major_version).arg(minor_version).arg(data_version);
        }
    }
}

bool
JouleDevice::getUnitFreeSpace(QString &memory, QString &err)
{
    statusCallback("Get Unit Free Space...");
    if (JOULE_DEBUG) printf("Get Unit Free Space\n");

    JoulePacket request1(GET_FREE_SPACE);

    if (!request1.write(dev, err)) return false;

    JoulePacket response1 = JoulePacket(GET_FREE_SPACE);
    if (response1.read(dev, err)) {

        if (response1.payload.length()>3) {
            int empty = qByteArray2Int(response1.payload.left(2));
            int total = qByteArray2Int(response1.payload.right(2));
            int percentage = 100 * empty / total;
            memory = QString("%1/%2 (%3%)").arg(empty).arg(total).arg(percentage);
        }
    }
}

bool
JouleDevice::getSystemInfo(QString &system, QByteArray &array, QString &err)
{
    statusCallback("Get System info...");
    if (JOULE_DEBUG) printf("Get System info\n");

    JoulePacket request(READ_SYSTEM_INFO);

    if (!request.write(dev, err)) return false;

    JoulePacket response = JoulePacket(READ_SYSTEM_INFO);
    if (response.read(dev, err)) {

        if (response.payload.length()>3) {
            array = response.dataArray();
            int serial = qByteArray2Int(response.payload.left(4));
            system = QString("%1").arg(serial);
        }
    }
}

bool
JouleDevice::getDownloadableRides(QList<DeviceStoredRideItem> &rides, QString &err)
{
    statusCallback("Read ride summary...");
    if (JOULE_DEBUG) printf("Get Unit Software Version\n");

    JoulePacket request(READ_RIDE_SUMMARY);

    if (!request.write(dev, err)) return false;

    JoulePacket response = JoulePacket(READ_RIDE_SUMMARY);
    if (response.read(dev, err)) {
        int count = response.payload.length()/20;

        for (int i=0; i<count; i++) {
            int j = i*20;
            int sec = bcd2Int(response.payload.at(j));
            int min = bcd2Int(response.payload.at(j+1));
            int hour = bcd2Int(response.payload.at(j+2));
            int day = bcd2Int(response.payload.at(j+3));
            int month = bcd2Int(response.payload.at(j+4))-1;
            int year = 2000 + bcd2Int(response.payload.at(j+5));

            QDateTime date = QDateTime(QDate(year,month,day), QTime(hour,min,sec));

            int total = qByteArray2Int(response.payload.mid(j+18,2));

            if (total > 0) {
                DeviceStoredRideItem ride;
                ride.id = i;
                ride.startTime = date;
                rides.append(ride);
            }
        }
        return true;
    }
    return false;
}

bool
JouleDevice::cleanup( QString &err ) {
    if (JOULE_DEBUG) printf("Erase all records on computer\n");

    if (!dev->open(err)) {
        err = "ERROR: open failed: " + err;
    }

    dev->setBaudRate(57600, err);

    QString version = QString("");
    QString serial = QString("");
    QByteArray versionArray;
    QByteArray systemInfoArray;

    getUnitVersion(version, versionArray, err);
    statusCallback("Version"+version);

    getSystemInfo(serial, systemInfoArray, err);

    QList<DeviceStoredRideItem> trainings;
    if (!getDownloadableRides(trainings, err))
        return false;

    for (int i=0; i<trainings.count(); i++) {
        statusCallback(QString("Delete ride detail for ride %1/%2").arg(i+1).arg(trainings.count()));
        JoulePacket request(ERASE_RIDE_DETAIL);
        int id1 = (trainings.at(i).id>255?trainings.at(i).id-255:trainings.at(i).id);
        int id2 = (trainings.at(i).id>255?trainings.at(i).id%255:0);
        request.addToPayload((char)id1);
        request.addToPayload((char)id2);
        request.addToPayload((char)id1);
        request.addToPayload((char)id2);

        if (!request.write(dev, err)) return false;

        JoulePacket response = JoulePacket(ERASE_RIDE_DETAIL);
        if (!response.read(dev, err))
            return false;
    }

    dev->close();

    return true;
}

// --------------------------------------
// JoulePacket
// --------------------------------------

JoulePacket::JoulePacket() : command(0)
{
    checksum = 0;
    payload.clear();
}

JoulePacket::JoulePacket(uint16_t command) : command(command), checksum(command)
{
    //checksum = 0;
    checksum += (uint8_t)((command & 0xFF00) >> 8);
    //checksum += (uint8_t)(command & 0x00FF);
    payload.clear();
}

void
JoulePacket::addToPayload(char byte)
{
    checksum += byte;
    payload.append(byte);
}

void
JoulePacket::addToPayload(uint16_t bytes)
{
    payload.append(bytes >> 8);
    payload.append(bytes & 0x00FF);
    checksum += (uint8_t)(bytes >> 8);
    checksum += (uint8_t)(bytes & 0x00FF);
}

void
JoulePacket::addToPayload(char *bytes, int len)
{
    for (int i = 0; i < len; ++i) {
        char byte = bytes[i];
        checksum += byte;
        payload.append(byte);
    }
}

bool
JoulePacket::verifyCheckSum(CommPortPtr dev, QString &err)
{
    char _checksum;
    if (JOULE_DEBUG) printf("reading checksum from device\n");
    int n = dev->read(&_checksum, 1, err);
    if (n <= 0) {
        err = (n < 0) ? ("read checksum error: " + err) : "read timeout";
        return false;
    }
    if (JOULE_DEBUG) printf("CheckSum1 %d CheckSum2 %d", (0xff & (unsigned) checksum) , (0xff & (unsigned) _checksum));

    return checksum == _checksum;
}

QByteArray
JoulePacket::dataArray()
{
    QByteArray result;
    result.append(char(START_1));
    result.append(char(START_2));

    result.append((char)(command & 0x00FF));
    result.append((char)((command & 0xFF00) >> 8));

    char lenght1 = (char)(payload.length() & 0x00FF);
    char lenght2 = (char)((payload.length() & 0xFF00) >> 8);
    result.append(lenght1);
    result.append(lenght2);

    result.append(payload);

    result.append(checksum+lenght1+lenght2);

    return result;
}

QByteArray
JoulePacket::dataArrayForUnit()
{
    QByteArray result = dataArray();

    for (int i = 2; i < result.size(); i++)
    {
        if (result.at(i) == START_1) {
            result.insert(i, START_1);
            i++;
        }
    }

    return result;
}

bool
JoulePacket::write(CommPortPtr dev, QString &err)
{
    QByteArray bytes = dataArrayForUnit();
    const char *msg = cEscape(bytes.data(), bytes.count()).toAscii().constData();

    if (JOULE_DEBUG) printf("writing '%s' to device\n", msg);

    int n = dev->write(bytes.data(), bytes.count() , err); //
    if (n != bytes.count()) {
        if (n < 0) {
            if (JOULE_DEBUG) printf("failed to write %s to device: %s\n", msg, err.toAscii().constData());
            err = QString("failed to write to device: %1").arg(err);
        }
        else {
            if (JOULE_DEBUG) printf("timeout writing %s to device\n", msg);
            err = QString("timeout writing to device");
        }
        return false;
    }

    if (JOULE_DEBUG) printf("writing to device ok\n");
    return true;
}



bool
JoulePacket::read(CommPortPtr dev, QString &err)
{
    if (JOULE_DEBUG) printf("reading from device\n");
    int n = dev->read(&startpattern, 2, err);
    if (n <= 0) {
        err = (n < 0) ? ("read header error: " + err) : "read timeout";
        return false;
    }

    uint16_t _command = 0;
    n = readOneByOne(dev, &_command, 2, err);
    if (n <= 0) {
        err = (n < 0) ? ("read command error: " + err) : "read timeout";
        return false;
    } else if (_command != command) {
        err = "wrong response";
        return false;
    }
    //if (JOULE_DEBUG) printf("command %s\n" , command);
    // command already in checksum

    n = readOneByOne(dev, &length, 2, err);
    if (n <= 0) {
        err = (n < 0) ? ("read length error: " + err) : "read timeout";
        return false;
    }
    if (JOULE_DEBUG) printf("length %d\n" ,length);
    checksum += (uint8_t)(length >> 8);
    checksum += (uint8_t)(length & 0x00FF);


    char buf[length];
    n = readOneByOne(dev, &buf, length, err);

    if (n <= 0) {
        err = (n < 0) ? ("read error: " + err) : "read timeout";
        return false;
    } else if (n < length) {
        err += QString(", read only %1 bytes insteed of: %2")
            .arg(n).arg(length);
        return false;
    }

    //if (JOULE_DEBUG) printf("payload %s\n" ,cEscape(buf,n).toAscii().constData());
    addToPayload(buf,n);

    char _checksum;
    n = readOneByOne(dev, &_checksum, 1, err);
    if (n <= 0) {
        err = (n < 0) ? ("read checksum error: " + err) : "read timeout";
        return false;
    } else if (_checksum != checksum) {
        err = "wrong _checksum";
        return false;
    }

    return true;
}
