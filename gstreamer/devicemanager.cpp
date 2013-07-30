/*  This file is part of the KDE project.

    Copyright (C) 2009 Nokia Corporation and/or its subsidiary(-ies).

    This library is free software: you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation, either version 2.1 or 3 of the License.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this library.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "devicemanager.h"

#include <gst/interfaces/propertyprobe.h>
#include "phonon-config-gstreamer.h" // krazy:exclude=includes
#include "backend.h"
#include "debug.h"
#include "gsthelper.h"
#include "videowidget.h"
#include "audiooutput.h"
#include <phonon/pulsesupport.h>

#include <QtCore/QSettings>

#ifdef HAVE_UDEV
#include <libudev.h>
#endif // HAVE_UDEV

#ifdef HAVE_ALSA
#include <alsa/asoundlib.h>
#endif // HAVE_ALSA

/*
 * This class manages the list of currently
 * active output devices
 */

namespace Phonon
{
namespace Gstreamer
{

/*
 * Device Info
 */

DeviceInfo::DeviceInfo(DeviceManager *manager, const QByteArray &deviceId,
                       quint16 caps, bool isAdvanced)
        : m_isAdvanced(isAdvanced), m_capabilities(caps)
{
    // Get an unique integer id for each device
    static int deviceCounter = 0;
    m_id = deviceCounter ++;

    if (caps & VideoCapture) {
        m_name = deviceId;
        m_description = deviceId;
    }

    if (caps & AudioOutput) {
        // This should never be called when PulseAudio is active.
        Q_ASSERT(!PulseSupport::getInstance()->isActive());

        // Get a preferred name from the device
        if (deviceId == "default") {
            m_name = "Default";
            m_description = "Default audio device";
        } else {
            GstElement *aSink = AudioOutput::createAudioSink();
            useGstElement(aSink, deviceId);
        }
    }

    // A default device should never be advanced
    if (deviceId == "default")
        m_isAdvanced = false;
}

void DeviceInfo::useGstElement(GstElement *element, const QByteArray &deviceId)
{
    if (!element)
        return;

    gchar *deviceName = NULL;
    if (GST_IS_PROPERTY_PROBE(element) && gst_property_probe_get_property(GST_PROPERTY_PROBE(element), "device")) {
        g_object_set(G_OBJECT(element), "device", deviceId.constData(), NULL);
        g_object_get(G_OBJECT(element), "device-name", &deviceName, NULL);
        m_name = QString(deviceName);

        if (m_description.isEmpty()) {
            // Construct a description by using the factory name and the device id
            GstElementFactory *factory = gst_element_get_factory(element);
            const gchar *factoryName = gst_element_factory_get_longname(factory);
            m_description = QString(factoryName) + ": " + deviceId;
        }

        g_free(deviceName);
        gst_element_set_state(element, GST_STATE_NULL);
        gst_object_unref(element);
    }
}

int DeviceInfo::id() const
{
    return m_id;
}

const QString& DeviceInfo::name() const
{
    return m_name;
}

const QString& DeviceInfo::description() const
{
    return m_description;
}

bool DeviceInfo::isAdvanced() const
{
    return m_isAdvanced;
}

void DeviceInfo::setAdvanced(bool advanced)
{
    m_isAdvanced = advanced;
}

const DeviceAccessList& DeviceInfo::accessList() const
{
    return m_accessList;
}

void DeviceInfo::addAccess(const DeviceAccess& access)
{
    m_accessList.append(access);
}

quint16 DeviceInfo::capabilities() const
{
    return m_capabilities;
}

void DeviceInfo::setCapabilities(quint16 cap)
{
    m_capabilities = cap;
}


/*
 * Device Manager
 */

DeviceManager::DeviceManager(QObject *parent)
        : QObject(parent)
{
    updateDeviceList();
}

DeviceManager::~DeviceManager()
{
}

QList<int> DeviceManager::deviceIds(ObjectDescriptionType type)
{
    DeviceInfo::Capability capability = DeviceInfo::None;
    switch (type) {
    case Phonon::AudioOutputDeviceType:
        capability = DeviceInfo::AudioOutput;
        break;
    case Phonon::AudioCaptureDeviceType:
        capability = DeviceInfo::AudioCapture;
        break;
    case Phonon::VideoCaptureDeviceType:
        capability = DeviceInfo::VideoCapture;
        break;
    default: ;
    }

    QList<int> ids;
    foreach (const DeviceInfo &device, m_devices) {
        if (device.capabilities() & capability)
            ids.append(device.id());
    }

    return ids;
}

QHash<QByteArray, QVariant> DeviceManager::deviceProperties(int id)
{
    QHash<QByteArray, QVariant> properties;

    foreach (const DeviceInfo &device, m_devices) {
        if (device.id() == id) {
            properties.insert("name", device.name());
            properties.insert("description", device.description());
            properties.insert("isAdvanced", device.isAdvanced());
            properties.insert("deviceAccessList", QVariant::fromValue<Phonon::DeviceAccessList>(device.accessList()));
            properties.insert("discovererIcon", QLatin1String("phonon-gstreamer"));

            if (device.capabilities() & DeviceInfo::AudioOutput) {
                properties.insert("icon", QLatin1String("audio-card"));
            }

            if (device.capabilities() & DeviceInfo::AudioCapture) {
                properties.insert("hasaudio", true);
                properties.insert("icon", QLatin1String("audio-input-microphone"));
            }

            if (device.capabilities() & DeviceInfo::VideoCapture) {
                properties.insert("hasvideo", true);
                properties.insert("icon", QLatin1String("camera-web"));
            }
            break;
        }
    }

    return properties;
}

/**
 * \param id The identifier for the device
 * \return Pointer to DeviceInfo, or NULL if the id is invalid
 */
const DeviceInfo *DeviceManager::device(int id) const
{
    for (int i = 0; i < m_devices.size(); i ++) {
        if (m_devices[i].id() == id)
            return &m_devices[i];
    }

    return NULL;
}

/**
 * Updates the current list of active devices
 */
void DeviceManager::updateDeviceList()
{
    QList<DeviceInfo> newDeviceList;
    QList<QByteArray> names;

    /*
     * Audio output
     */
    if (!PulseSupport::getInstance()->isActive()) {
#ifdef HAVE_ALSA
      int card = -1;
      while (snd_card_next(&card) == 0 && card > -1) {
          int err;
          QString devName;
          QString friendlyName;
          bool output = false;
          bool input = false;
          char *deviceHints;
          devName = QString("hw:%1").arg(card);

          snd_ctl_t *ctl_handle;
          snd_ctl_open(&ctl_handle, devName.toAscii().constData(), 0);
          snd_ctl_card_info_t *hw_info;
          snd_ctl_card_info_alloca(&hw_info);
          if ((err = snd_ctl_card_info(ctl_handle, hw_info) == 0)) {
              friendlyName = snd_ctl_card_info_get_name(hw_info);
          } else {
              friendlyName = devName;
          }
          qDebug() << "Inspecting card" << devName << friendlyName;
          snd_ctl_close(ctl_handle);
          deviceHints = snd_device_name_get_hint(devName.toAscii().constData(), "IOID");
          if (deviceHints == NULL) {
              input = true;
              output = true;
          } else if (strcmp(deviceHints, "Input")) {
              input = true;
          } else if (strcmp(deviceHints, "Output")) {
              output = true;
          }
          free(deviceHints);
          if (output || input) {
              if (output) {
                  DeviceInfo deviceInfo(this, friendlyName.toUtf8(), DeviceInfo::AudioOutput);
                  deviceInfo.addAccess(DeviceAccess("alsa", devName));
                  newDeviceList.append(deviceInfo);
              }
              if (input) {
                  DeviceInfo deviceInfo(this, friendlyName.toUtf8(), DeviceInfo::AudioCapture);
                  deviceInfo.addAccess(DeviceAccess("alsa", devName));
                  newDeviceList.append(deviceInfo);
              }
          }
      }
#endif // HAVE_ALSA
    }

    /*
     * Video capture
     */
#ifdef HAVE_UDEV

    struct udev *ctx;
    struct udev_enumerate *iter;
    struct udev_list_entry *devlist;
    ctx = udev_new ();
    iter = udev_enumerate_new (ctx);
    udev_enumerate_add_match_subsystem (iter, "video4linux");
    udev_enumerate_scan_devices (iter);

    devlist = udev_enumerate_get_list_entry (iter);

    while (devlist) {
      const char *sysname = udev_list_entry_get_name (devlist);
      struct udev_device *dev = udev_device_new_from_syspath (ctx, sysname);
      const char *devfile = udev_device_get_devnode (dev);
      const char *devname = udev_device_get_sysattr_value (dev, "name");
      if (devfile) {
        DeviceInfo deviceInfo(this, devname, DeviceInfo::VideoCapture);
        deviceInfo.addAccess(DeviceAccess("v4l2", devfile));
        newDeviceList.append(deviceInfo);
      }
      udev_device_unref (dev);
      devlist = udev_list_entry_get_next (devlist);
    }

    udev_enumerate_unref (iter);
    udev_unref (ctx);

#endif // HAVE_UDEV

    /*
     * Compares the list with the devices available at the moment with the last list. If
     * a new device is seen, a signal is emitted. If a device dissapeared, another signal
     * is emitted.
     */

    // Search for added devices
    for (int i = 0; i < newDeviceList.count(); ++i) {
        int id = newDeviceList[i].id();
        if (!listContainsDevice(m_devices, id)) {
            // This is a new device, add it
            m_devices.append(newDeviceList[i]);
            emit deviceAdded(id);

            debug() << "Found new device" << newDeviceList[i].name();
        }
    }

    // Search for removed devices
    for (int i = m_devices.count() - 1; i >= 0; --i) {
        int id = m_devices[i].id();
        if (!listContainsDevice(newDeviceList, id)) {
            debug() << "Lost device" << m_devices[i].name();

            emit deviceRemoved(id);
            m_devices.removeAt(i);
        }
    }
}

bool DeviceManager::listContainsDevice(const QList<DeviceInfo> &list, int id)
{
    foreach (const DeviceInfo &d, list) {
        if (d.id() == id)
            return true;
    }
    return false;
}

}
}
