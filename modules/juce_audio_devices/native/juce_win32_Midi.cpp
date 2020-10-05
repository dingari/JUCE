/*
  ==============================================================================

   This file is part of the JUCE library.
   Copyright (c) 2020 - Raw Material Software Limited

   JUCE is an open source library subject to commercial or open-source
   licensing.

   The code included in this file is provided under the terms of the ISC license
   http://www.isc.org/downloads/software-support-policy/isc-license. Permission
   To use, copy, modify, and/or distribute this software for any purpose with or
   without fee is hereby granted provided that the above copyright notice and
   this permission notice appear in all copies.

   JUCE IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL WARRANTIES, WHETHER
   EXPRESSED OR IMPLIED, INCLUDING MERCHANTABILITY AND FITNESS FOR PURPOSE, ARE
   DISCLAIMED.

  ==============================================================================
*/

#ifndef DRV_QUERYDEVICEINTERFACE
 #define DRV_RESERVED                  0x0800
 #define DRV_QUERYDEVICEINTERFACE     (DRV_RESERVED + 12)
 #define DRV_QUERYDEVICEINTERFACESIZE (DRV_RESERVED + 13)
#endif

#if JUCE_USE_WINRT_MIDI
#include <combaseapi.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Midi.h>

// Avoid clases with the WRL namespaces
// This file must be included somewhere else...
namespace winrt_wrap {
using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Devices;
using namespace winrt::Windows::Devices::Enumeration;
using namespace winrt::Windows::Devices::Radios;
using namespace winrt::Windows::Devices::Midi;
using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
}
#endif

namespace juce
{

struct MidiServiceType
{
    struct InputWrapper
    {
        virtual ~InputWrapper() {}

        virtual String getDeviceIdentifier() = 0;
        virtual String getDeviceName() = 0;

        virtual void start() = 0;
        virtual void stop() = 0;
    };

    struct OutputWrapper
    {
        virtual ~OutputWrapper() {}

        virtual String getDeviceIdentifier() = 0;
        virtual String getDeviceName() = 0;

        virtual void sendMessageNow (const MidiMessage&) = 0;
    };

    MidiServiceType() {}
    virtual ~MidiServiceType() {}

    virtual Array<MidiDeviceInfo> getAvailableDevices (bool) = 0;
    virtual MidiDeviceInfo getDefaultDevice (bool) = 0;

    virtual InputWrapper*  createInputWrapper  (MidiInput&, const String&, MidiInputCallback&) = 0;
    virtual OutputWrapper* createOutputWrapper (const String&) = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiServiceType)
};

//==============================================================================
struct Win32MidiService  : public MidiServiceType,
                           private Timer
{
    Win32MidiService() {}

    Array<MidiDeviceInfo> getAvailableDevices (bool isInput) override
    {
        return isInput ? Win32InputWrapper::getAvailableDevices()
                       : Win32OutputWrapper::getAvailableDevices();
    }

    MidiDeviceInfo getDefaultDevice (bool isInput) override
    {
        return isInput ? Win32InputWrapper::getDefaultDevice()
                       : Win32OutputWrapper::getDefaultDevice();
    }

    InputWrapper* createInputWrapper (MidiInput& input, const String& deviceIdentifier, MidiInputCallback& callback) override
    {
        return new Win32InputWrapper (*this, input, deviceIdentifier, callback);
    }

    OutputWrapper* createOutputWrapper (const String& deviceIdentifier) override
    {
        return new Win32OutputWrapper (*this, deviceIdentifier);
    }

private:
    struct Win32InputWrapper;

    //==============================================================================
    struct MidiInCollector  : public ReferenceCountedObject
    {
        MidiInCollector (Win32MidiService& s, MidiDeviceInfo d)
            : deviceInfo (d), midiService (s)
        {
        }

        ~MidiInCollector()
        {
            stop();

            if (deviceHandle != 0)
            {
                for (int count = 5; --count >= 0;)
                {
                    if (midiInClose (deviceHandle) == MMSYSERR_NOERROR)
                        break;

                    Sleep (20);
                }
            }
        }

        using Ptr = ReferenceCountedObjectPtr<MidiInCollector>;

        void addClient (Win32InputWrapper* c)
        {
            const ScopedLock sl (clientLock);
            jassert (! clients.contains (c));
            clients.add (c);
        }

        void removeClient (Win32InputWrapper* c)
        {
            const ScopedLock sl (clientLock);
            clients.removeFirstMatchingValue (c);
            startOrStop();
            midiService.asyncCheckForUnusedCollectors();
        }

        void handleMessage (const uint8* bytes, uint32 timeStamp)
        {
            if (bytes[0] >= 0x80 && isStarted.load())
            {
                {
                    auto len = MidiMessage::getMessageLengthFromFirstByte (bytes[0]);
                    auto time = convertTimeStamp (timeStamp);
                    const ScopedLock sl (clientLock);

                    for (auto* c : clients)
                        c->pushMidiData (bytes, len, time);
                }

                writeFinishedBlocks();
            }
        }

        void handleSysEx (MIDIHDR* hdr, uint32 timeStamp)
        {
            if (isStarted.load() && hdr->dwBytesRecorded > 0)
            {
                {
                    auto time = convertTimeStamp (timeStamp);
                    const ScopedLock sl (clientLock);

                    for (auto* c : clients)
                        c->pushMidiData (hdr->lpData, (int) hdr->dwBytesRecorded, time);
                }

                writeFinishedBlocks();
            }
        }

        void startOrStop()
        {
            const ScopedLock sl (clientLock);

            if (countRunningClients() == 0)
                stop();
            else
                start();
        }

        void start()
        {
            if (deviceHandle != 0 && ! isStarted.load())
            {
                activeMidiCollectors.addIfNotAlreadyThere (this);

                for (int i = 0; i < (int) numHeaders; ++i)
                {
                    headers[i].prepare (deviceHandle);
                    headers[i].write (deviceHandle);
                }

                startTime = Time::getMillisecondCounterHiRes();
                auto res = midiInStart (deviceHandle);

                if (res == MMSYSERR_NOERROR)
                    isStarted = true;
                else
                    unprepareAllHeaders();
            }
        }

        void stop()
        {
            if (isStarted.load())
            {
                isStarted = false;
                midiInReset (deviceHandle);
                midiInStop (deviceHandle);
                activeMidiCollectors.removeFirstMatchingValue (this);
                unprepareAllHeaders();
            }
        }

        static void CALLBACK midiInCallback (HMIDIIN, UINT uMsg, DWORD_PTR dwInstance,
                                             DWORD_PTR midiMessage, DWORD_PTR timeStamp)
        {
            auto* collector = reinterpret_cast<MidiInCollector*> (dwInstance);

            // This is primarily a check for the collector being a dangling
            // pointer, as the callback can sometimes be delayed
            if (activeMidiCollectors.contains (collector))
            {
                if (uMsg == MIM_DATA)
                    collector->handleMessage ((const uint8*) &midiMessage, (uint32) timeStamp);
                else if (uMsg == MIM_LONGDATA)
                    collector->handleSysEx ((MIDIHDR*) midiMessage, (uint32) timeStamp);
            }
        }

        MidiDeviceInfo deviceInfo;
        HMIDIIN deviceHandle = 0;

    private:
        Win32MidiService& midiService;
        CriticalSection clientLock;
        Array<Win32InputWrapper*> clients;
        std::atomic<bool> isStarted { false };
        double startTime = 0;

        // This static array is used to prevent occasional callbacks to objects that are
        // in the process of being deleted
        static Array<MidiInCollector*, CriticalSection> activeMidiCollectors;

        int countRunningClients() const
        {
            int num = 0;

            for (auto* c : clients)
                if (c->started)
                    ++num;

            return num;
        }

        struct MidiHeader
        {
            MidiHeader() {}

            void prepare (HMIDIIN device)
            {
                zerostruct (hdr);
                hdr.lpData = data;
                hdr.dwBufferLength = (DWORD) numElementsInArray (data);

                midiInPrepareHeader (device, &hdr, sizeof (hdr));
            }

            void unprepare (HMIDIIN device)
            {
                if ((hdr.dwFlags & WHDR_DONE) != 0)
                {
                    int c = 10;
                    while (--c >= 0 && midiInUnprepareHeader (device, &hdr, sizeof (hdr)) == MIDIERR_STILLPLAYING)
                        Thread::sleep (20);

                    jassert (c >= 0);
                }
            }

            void write (HMIDIIN device)
            {
                hdr.dwBytesRecorded = 0;
                midiInAddBuffer (device, &hdr, sizeof (hdr));
            }

            void writeIfFinished (HMIDIIN device)
            {
                if ((hdr.dwFlags & WHDR_DONE) != 0)
                    write (device);
            }

            MIDIHDR hdr;
            char data[256];

            JUCE_DECLARE_NON_COPYABLE (MidiHeader)
        };

        enum { numHeaders = 32 };
        MidiHeader headers[numHeaders];

        void writeFinishedBlocks()
        {
            for (int i = 0; i < (int) numHeaders; ++i)
                headers[i].writeIfFinished (deviceHandle);
        }

        void unprepareAllHeaders()
        {
            for (int i = 0; i < (int) numHeaders; ++i)
                headers[i].unprepare (deviceHandle);
        }

        double convertTimeStamp (uint32 timeStamp)
        {
            auto t = startTime + timeStamp;
            auto now = Time::getMillisecondCounterHiRes();

            if (t > now)
            {
                if (t > now + 2.0)
                    startTime -= 1.0;

                t = now;
            }

            return t * 0.001;
        }

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiInCollector)
    };

    //==============================================================================
    template<class WrapperType>
    struct Win32MidiDeviceQuery
    {
        static Array<MidiDeviceInfo> getAvailableDevices()
        {
            StringArray deviceNames, deviceIDs;
            auto deviceCaps = WrapperType::getDeviceCaps();

            for (int i = 0; i < deviceCaps.size(); ++i)
            {
                deviceNames.add (deviceCaps[i].szPname);

                auto identifier = getInterfaceIDForDevice ((UINT) i);

                if (identifier.isNotEmpty())
                    deviceIDs.add (identifier);
                else
                    deviceIDs.add (deviceNames[i]);
            }

            deviceNames.appendNumbersToDuplicates (false, false, CharPointer_UTF8 ("-"), CharPointer_UTF8 (""));
            deviceIDs  .appendNumbersToDuplicates (false, false, CharPointer_UTF8 ("-"), CharPointer_UTF8 (""));

            Array<MidiDeviceInfo> devices;

            for (int i = 0; i < deviceNames.size(); ++i)
                devices.add ({ deviceNames[i], deviceIDs[i] });

            return devices;
        }

    private:
        static String getInterfaceIDForDevice (UINT id)
        {
            ULONG size = 0;

            if (WrapperType::sendMidiMessage ((UINT_PTR) id, DRV_QUERYDEVICEINTERFACESIZE, (DWORD_PTR) &size, 0) == MMSYSERR_NOERROR)
            {
                WCHAR interfaceName[512] = {};

                if (isPositiveAndBelow (size, sizeof (interfaceName))
                    && WrapperType::sendMidiMessage ((UINT_PTR) id, DRV_QUERYDEVICEINTERFACE,
                                                     (DWORD_PTR) interfaceName, sizeof (interfaceName)) == MMSYSERR_NOERROR)
                {
                    return interfaceName;
                }
            }

            return {};
        }
    };

    struct Win32InputWrapper  : public InputWrapper,
                                public Win32MidiDeviceQuery<Win32InputWrapper>
    {
        Win32InputWrapper (Win32MidiService& parentService, MidiInput& midiInput, const String& deviceIdentifier, MidiInputCallback& c)
            : input (midiInput), callback (c)
        {
            collector = getOrCreateCollector (parentService, deviceIdentifier);
            collector->addClient (this);
        }

        ~Win32InputWrapper()
        {
            collector->removeClient (this);
        }

        static MidiInCollector::Ptr getOrCreateCollector (Win32MidiService& parentService, const String& deviceIdentifier)
        {
            UINT deviceID = MIDI_MAPPER;
            String deviceName;
            auto devices = getAvailableDevices();

            for (int i = 0; i < devices.size(); ++i)
            {
                auto d = devices.getUnchecked (i);

                if (d.identifier == deviceIdentifier)
                {
                    deviceID = i;
                    deviceName = d.name;
                    break;
                }
            }

            const ScopedLock sl (parentService.activeCollectorLock);

            for (auto& c : parentService.activeCollectors)
                if (c->deviceInfo.identifier == deviceIdentifier)
                    return c;

            MidiInCollector::Ptr c (new MidiInCollector (parentService, { deviceName, deviceIdentifier }));

            HMIDIIN h;
            auto err = midiInOpen (&h, deviceID,
                                   (DWORD_PTR) &MidiInCollector::midiInCallback,
                                   (DWORD_PTR) (MidiInCollector*) c.get(),
                                   CALLBACK_FUNCTION);

            if (err != MMSYSERR_NOERROR)
                throw std::runtime_error ("Failed to create Windows input device wrapper");

            c->deviceHandle = h;
            parentService.activeCollectors.add (c);
            return c;
        }

        static DWORD sendMidiMessage (UINT_PTR deviceID, UINT msg, DWORD_PTR arg1, DWORD_PTR arg2)
        {
            return midiInMessage ((HMIDIIN) deviceID, msg, arg1, arg2);
        }

        static Array<MIDIINCAPS> getDeviceCaps()
        {
            Array<MIDIINCAPS> devices;

            for (UINT i = 0; i < midiInGetNumDevs(); ++i)
            {
                MIDIINCAPS mc = {};

                if (midiInGetDevCaps (i, &mc, sizeof (mc)) == MMSYSERR_NOERROR)
                    devices.add (mc);
            }

            return devices;
        }

        static MidiDeviceInfo getDefaultDevice()  { return getAvailableDevices().getFirst(); }

        void start() override   { started = true;  concatenator.reset(); collector->startOrStop(); }
        void stop() override    { started = false; collector->startOrStop(); concatenator.reset(); }

        String getDeviceIdentifier() override   { return collector->deviceInfo.identifier; }
        String getDeviceName() override         { return collector->deviceInfo.name; }

        void pushMidiData (const void* inputData, int numBytes, double time)
        {
            concatenator.pushMidiData (inputData, numBytes, time, &input, callback);
        }

        MidiInput& input;
        MidiInputCallback& callback;
        MidiDataConcatenator concatenator { 4096 };
        MidiInCollector::Ptr collector;
        bool started = false;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Win32InputWrapper)
    };

    //==============================================================================
    struct MidiOutHandle    : public ReferenceCountedObject
    {
        using Ptr = ReferenceCountedObjectPtr<MidiOutHandle>;

        MidiOutHandle (Win32MidiService& parent, MidiDeviceInfo d, HMIDIOUT h)
            : owner (parent), deviceInfo (d), handle (h)
        {
            owner.activeOutputHandles.add (this);
        }

        ~MidiOutHandle()
        {
            if (handle != nullptr)
                midiOutClose (handle);

            owner.activeOutputHandles.removeFirstMatchingValue (this);
        }

        Win32MidiService& owner;
        MidiDeviceInfo deviceInfo;
        HMIDIOUT handle;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiOutHandle)
    };

    //==============================================================================
    struct Win32OutputWrapper  : public OutputWrapper,
                                 public Win32MidiDeviceQuery<Win32OutputWrapper>
    {
        Win32OutputWrapper (Win32MidiService& p, const String& deviceIdentifier)
            : parent (p)
        {
            auto devices = getAvailableDevices();
            UINT deviceID = MIDI_MAPPER;
            String deviceName;

            for (int i = 0; i < devices.size(); ++i)
            {
                auto d = devices.getUnchecked (i);

                if (d.identifier == deviceIdentifier)
                {
                    deviceID = i;
                    deviceName = d.name;
                    break;
                }
            }

            if (deviceID == MIDI_MAPPER)
            {
                // use the microsoft sw synth as a default - best not to allow deviceID
                // to be MIDI_MAPPER, or else device sharing breaks
                for (int i = 0; i < devices.size(); ++i)
                    if (devices[i].name.containsIgnoreCase ("microsoft"))
                        deviceID = (UINT) i;
            }

            for (int i = parent.activeOutputHandles.size(); --i >= 0;)
            {
                auto* activeHandle = parent.activeOutputHandles.getUnchecked (i);

                if (activeHandle->deviceInfo.identifier == deviceIdentifier)
                {
                    han = activeHandle;
                    return;
                }
            }

            for (int i = 4; --i >= 0;)
            {
                HMIDIOUT h = 0;
                auto res = midiOutOpen (&h, deviceID, 0, 0, CALLBACK_NULL);

                if (res == MMSYSERR_NOERROR)
                {
                    han = new MidiOutHandle (parent, { deviceName, deviceIdentifier }, h);
                    return;
                }

                if (res == MMSYSERR_ALLOCATED)
                    Sleep (100);
                else
                    break;
            }

            throw std::runtime_error ("Failed to create Windows output device wrapper");
        }

        void sendMessageNow (const MidiMessage& message) override
        {
            if (message.getRawDataSize() > 3 || message.isSysEx())
            {
                MIDIHDR h = {};

                h.lpData = (char*) message.getRawData();
                h.dwBytesRecorded = h.dwBufferLength  = (DWORD) message.getRawDataSize();

                if (midiOutPrepareHeader (han->handle, &h, sizeof (MIDIHDR)) == MMSYSERR_NOERROR)
                {
                    auto res = midiOutLongMsg (han->handle, &h, sizeof (MIDIHDR));

                    if (res == MMSYSERR_NOERROR)
                    {
                        while ((h.dwFlags & MHDR_DONE) == 0)
                            Sleep (1);

                        int count = 500; // 1 sec timeout

                        while (--count >= 0)
                        {
                            res = midiOutUnprepareHeader (han->handle, &h, sizeof (MIDIHDR));

                            if (res == MIDIERR_STILLPLAYING)
                                Sleep (2);
                            else
                                break;
                        }
                    }
                }
            }
            else
            {
                for (int i = 0; i < 50; ++i)
                {
                    if (midiOutShortMsg (han->handle, *(unsigned int*) message.getRawData()) != MIDIERR_NOTREADY)
                        break;

                    Sleep (1);
                }
            }
        }

        static DWORD sendMidiMessage (UINT_PTR deviceID, UINT msg, DWORD_PTR arg1, DWORD_PTR arg2)
        {
            return midiOutMessage ((HMIDIOUT) deviceID, msg, arg1, arg2);
        }

        static Array<MIDIOUTCAPS> getDeviceCaps()
        {
            Array<MIDIOUTCAPS> devices;

            for (UINT i = 0; i < midiOutGetNumDevs(); ++i)
            {
                MIDIOUTCAPS mc = {};

                if (midiOutGetDevCaps (i, &mc, sizeof (mc)) == MMSYSERR_NOERROR)
                    devices.add (mc);
            }

            return devices;
        }

        static MidiDeviceInfo getDefaultDevice()
        {
            auto defaultIndex = []()
            {
                auto deviceCaps = getDeviceCaps();

                for (int i = 0; i < deviceCaps.size(); ++i)
                    if ((deviceCaps[i].wTechnology & MOD_MAPPER) != 0)
                        return i;

                return 0;
            }();

            return getAvailableDevices()[defaultIndex];
        }

        String getDeviceIdentifier() override   { return han->deviceInfo.identifier; }
        String getDeviceName() override         { return han->deviceInfo.name; }

        Win32MidiService& parent;
        MidiOutHandle::Ptr han;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (Win32OutputWrapper)
    };

    //==============================================================================
    void asyncCheckForUnusedCollectors()
    {
        startTimer (10);
    }

    void timerCallback() override
    {
        stopTimer();

        const ScopedLock sl (activeCollectorLock);

        for (int i = activeCollectors.size(); --i >= 0;)
            if (activeCollectors.getObjectPointer(i)->getReferenceCount() == 1)
                activeCollectors.remove (i);
    }

    CriticalSection activeCollectorLock;
    ReferenceCountedArray<MidiInCollector> activeCollectors;
    Array<MidiOutHandle*> activeOutputHandles;
};

Array<Win32MidiService::MidiInCollector*, CriticalSection> Win32MidiService::MidiInCollector::activeMidiCollectors;

//==============================================================================
//==============================================================================
#if JUCE_USE_WINRT_MIDI

#ifndef JUCE_FORCE_WINRT_MIDI
 #define JUCE_FORCE_WINRT_MIDI 0
#endif

#ifndef JUCE_WINRT_MIDI_LOGGING
 #define JUCE_WINRT_MIDI_LOGGING 0
#endif

#if JUCE_WINRT_MIDI_LOGGING
 #define JUCE_WINRT_MIDI_LOG(x)  DBG(x)
#else
 #define JUCE_WINRT_MIDI_LOG(x)
#endif

//==============================================================================
struct WinRTMidiService  : public MidiServiceType
{
public:
    //==============================================================================
    WinRTMidiService()
    {
        // The WinRT BLE MIDI API doesn't provide callbacks when devices become disconnected,
        // but it does require a disconnection via the API before a device will reconnect again.
        // We can monitor the BLE connection state of paired devices to get callbacks when
        // connections are broken.
        bleDeviceWatcher = std::make_unique<BLEDeviceWatcher>();

        // TODO: Can this happen?
        if (! bleDeviceWatcher->start())
            throw std::runtime_error ("Failed to start the BLE device watcher");

        inputDeviceWatcher = std::make_unique<MidiIODeviceWatcher>(*bleDeviceWatcher, winrt_wrap::MidiInPort::GetDeviceSelector());

        // TODO: Can this happen?
        if (! inputDeviceWatcher->start())
            throw std::runtime_error ("Failed to start the midi input device watcher");

        outputDeviceWatcher = std::make_unique<MidiIODeviceWatcher>(*bleDeviceWatcher, winrt_wrap::MidiOutPort::GetDeviceSelector());

        // TODO: Can this happen?
        if (! outputDeviceWatcher->start())
            throw std::runtime_error ("Failed to start the midi output device watcher");
    }

    Array<MidiDeviceInfo> getAvailableDevices (bool isInput) override
    {
        return isInput ? inputDeviceWatcher ->getAvailableDevices()
                       : outputDeviceWatcher->getAvailableDevices();
    }

    MidiDeviceInfo getDefaultDevice (bool isInput) override
    {
        return isInput ? inputDeviceWatcher ->getDefaultDevice()
                       : outputDeviceWatcher->getDefaultDevice();
    }

    InputWrapper* createInputWrapper (MidiInput& input, const String& deviceIdentifier, MidiInputCallback& callback) override
    {
        return new WinRTInputWrapper (*this, input, deviceIdentifier, callback);
    }

    OutputWrapper* createOutputWrapper (const String& deviceIdentifier) override
    {
        return new WinRTOutputWrapper (*this, deviceIdentifier);
    }

private:
    //==============================================================================
    using PropertyStore = winrt_wrap::Collections::IMapView<winrt::hstring, winrt_wrap::IInspectable>;

    template<typename T>
    static auto getProperty(const PropertyStore& map, const winrt::hstring& key) -> std::optional<T>
    {
        return map.HasKey(key)
               ? std::optional(winrt::unbox_value<T>(map.Lookup(key)))
               : std::nullopt;
    }

    template<typename T>
    static auto getPropertyOr(const PropertyStore& map, const winrt::hstring& key, T def) -> T
    {
        const auto p = getProperty<T>(map, key);

        return p.has_value() ? *p : def;
    }

    //==============================================================================
    struct BLEDeviceWatcher final
    {
        struct DeviceInfo
        {
            String containerID{};
            bool isConnected = false;
        };

        BLEDeviceWatcher() : watcher(createWatcher())
        {
            watcher.Added([this](const winrt_wrap::DeviceWatcher&, const winrt_wrap::DeviceInformation& info)
            {
                const String deviceID(winrt::to_string(info.Id()));
                const String deviceName(winrt::to_string(info.Name()));

                JUCE_WINRT_MIDI_LOG ("Detected paired BLE device: " << deviceID << ", " << deviceName);

                const auto props = info.Properties();

                if (const auto id = getProperty<winrt::guid>(props, L"System.Devices.Aep.ContainerId"); id.has_value())
                {
                    if (const String id_str = winrt::to_string(winrt::to_hstring(*id)); id_str.isNotEmpty())
                    {
                        const DeviceInfo midi_info = {
                                id_str,
                                getPropertyOr<bool>(props, L"System.Devices.Aep.IsConnected", {})
                        };

                        JUCE_WINRT_MIDI_LOG("Adding BLE device: " << deviceID << " " << midi_info.containerID << ", name: " << deviceName
                                                                  << " " << (midi_info.isConnected ? "connected" : "disconnected"));

                        const ScopedLock lock(deviceChanges);
                        devices.set (deviceID, midi_info);
                    }
                }
            });

            watcher.Removed([this](const winrt_wrap::DeviceWatcher&, const winrt_wrap::DeviceInformationUpdate& info)
            {
               const auto removedDeviceId = String(winrt::to_string(info.Id()));

                JUCE_WINRT_MIDI_LOG ("Removing BLE device: " << removedDeviceId);

                if (devices.contains (removedDeviceId))
                {
                    const auto& midi_info = devices.getReference (removedDeviceId);
                    listeners.call ([&midi_info] (Listener& l) { l.bleDeviceDisconnected (midi_info.containerID); });
                    devices.remove (removedDeviceId);

                    JUCE_WINRT_MIDI_LOG ("Removed BLE device: " << removedDeviceId);
                }
            });

            watcher.Updated([this](const winrt_wrap::DeviceWatcher&, const winrt_wrap::DeviceInformationUpdate& info)
            {
                DBG("Device updated: " << String(winrt::to_string(info.Id())));

                const auto updatedDeviceId = String(winrt::to_string(info.Id()));

                if (const auto opt = getProperty<bool>(info.Properties(), L"System.Devices.Aep.IsConnected"); opt.has_value())
                {
                    const bool is_connected = *opt;

                    const ScopedLock lock (deviceChanges);

                    if (devices.contains(updatedDeviceId))
                    {
                        auto& midi_info = devices.getReference(updatedDeviceId);

                        if (midi_info.isConnected != is_connected)
                        {
                            JUCE_WINRT_MIDI_LOG ("BLE device connection status change: " << updatedDeviceId << " " << midi_info.containerID << " " << (is_connected ? "connected" : "disconnected"));

                            if (midi_info.isConnected && !is_connected)
                                listeners.call ([&midi_info] (Listener& l) { l.bleDeviceDisconnected (midi_info.containerID); });
                        }

                        midi_info.isConnected = is_connected;
                    }
                }
            });
        }

        static winrt_wrap::DeviceWatcher createWatcher()
        {
            const std::vector<winrt::hstring> props {
                    L"System.Devices.ContainerId",
                    L"System.Devices.Aep.ContainerId",
                    L"System.Devices.Aep.IsConnected"
            };

            // bb7bb05e-5972-42b5-94fc-76eaa7084d49 is the Bluetooth LE protocol ID, by the way...
            constexpr auto selector = L"System.Devices.Aep.ProtocolId:=\"{bb7bb05e-5972-42b5-94fc-76eaa7084d49}\""
                                      " AND System.Devices.Aep.IsPaired:=System.StructuredQueryType.Boolean#True";

            return winrt_wrap::DeviceInformation::CreateWatcher(selector, props, winrt_wrap::DeviceInformationKind::AssociationEndpoint);
        }

        //==============================================================================
        bool start()
        {
            watcher.Start();
            return true;
        }

        //==============================================================================
        struct Listener
        {
            virtual ~Listener() {};
            virtual void bleDeviceAdded (const String& containerID) = 0;
            virtual void bleDeviceDisconnected (const String& containerID) = 0;
        };

        void addListener (Listener* l) { listeners.add (l); }

        void removeListener (Listener* l) { listeners.remove (l); }

        //==============================================================================
        ListenerList<Listener> listeners;
        HashMap<String, DeviceInfo> devices;
        CriticalSection deviceChanges;

        winrt_wrap::DeviceWatcher watcher;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BLEDeviceWatcher);
    };

    //==============================================================================
    struct WinRTMIDIDeviceInfo
    {
        String deviceID{}, containerID{}, name{};
        bool isDefault = false;
    };

    //==============================================================================
    struct MidiIODeviceWatcher final
    {
        MidiIODeviceWatcher (const BLEDeviceWatcher& bleWatcher, const winrt::hstring& selectorString)
            : bleDeviceWatcher(bleWatcher),
              watcher(createWatcher(selectorString))
        {
            watcher.Added([this](const winrt_wrap::DeviceWatcher&, const winrt_wrap::DeviceInformation& info)
            {
                WinRTMIDIDeviceInfo midi_info{};
                midi_info.deviceID = winrt::to_string(info.Id());

                JUCE_WINRT_MIDI_LOG ("Detected MIDI device: " << midi_info.deviceID);

                if (!info.IsEnabled())
                {
                    JUCE_WINRT_MIDI_LOG ("MIDI device not enabled: " << midi_info.deviceID);
                    return;
                }

                if (const auto container_id = getProperty<winrt::guid>(info.Properties(), L"System.Devices.ContainerId"))
                    midi_info.containerID = winrt::to_string(winrt::to_hstring(*container_id));

                midi_info.name = winrt::to_string(info.Name());
                midi_info.isDefault = info.IsDefault();

                JUCE_WINRT_MIDI_LOG ("Adding MIDI device: " << midi_info.deviceID << " " << midi_info.containerID << " " << midi_info.name);

                const ScopedLock lock (deviceChanges);
                connectedDevices.add (midi_info);
            });

            watcher.Removed([this](const winrt_wrap::DeviceWatcher&, const winrt_wrap::DeviceInformationUpdate& info)
            {
                const String removedDeviceId(winrt::to_string(info.Id()));

                JUCE_WINRT_MIDI_LOG ("Removing MIDI device: " << removedDeviceId);

                const ScopedLock lock (deviceChanges);
                const auto it = std::remove_if(connectedDevices.begin(), connectedDevices.end(),
                        [&](const auto& d) { return d.deviceID == removedDeviceId; });

                connectedDevices.remove(it);
            });
        }

        static winrt_wrap::DeviceWatcher createWatcher(const winrt::hstring& selectorString)
        {
            const std::vector<winrt::hstring> props {
                    L"System.Devices.ContainerId",
                    L"System.Devices.Aep.ContainerId",
                    L"System.Devices.Aep.IsConnected"
            };

            return winrt_wrap::DeviceInformation::CreateWatcher(selectorString, props, winrt_wrap::DeviceInformationKind::DeviceInterface);
        }

        bool start()
        {
            watcher.Start();
            return true;
        }

        Array<MidiDeviceInfo> getAvailableDevices()
        {
            {
                const ScopedLock lock (deviceChanges);
                lastQueriedConnectedDevices = connectedDevices;
            }

            StringArray deviceNames, deviceIDs;

            // BLE MIDI devices that have been paired but are not currently connected to the system appear 
            // as MIDI I/O ports anyway. We use the container ID  to match the MIDI device with a generic 
            // BLE device and query it's connection status to see if it truly is available.
            const auto is_available = [&](const auto& device_info)
            {
                bool available = true;

                const ScopedLock lock(bleDeviceWatcher.deviceChanges);

                for (const auto& elem : bleDeviceWatcher.devices)
                {
                    if (elem.containerID == device_info.containerID)
                    {
                        available = elem.isConnected;
                        break;
                    }
                }

                return available;
            };

            for (const auto& info : lastQueriedConnectedDevices.get())
            {
                if (is_available(info))
                {
                    deviceNames.add (info.name);
                    deviceIDs  .add (info.containerID);
                }
            }

            deviceNames.appendNumbersToDuplicates (false, false, CharPointer_UTF8 ("-"), CharPointer_UTF8 (""));
            deviceIDs  .appendNumbersToDuplicates (false, false, CharPointer_UTF8 ("-"), CharPointer_UTF8 (""));

            Array<MidiDeviceInfo> devices;

            for (int i = 0; i < deviceNames.size(); ++i)
                devices.add ({ deviceNames[i], deviceIDs[i] });

            return devices;
        }

        MidiDeviceInfo getDefaultDevice()
        {
            auto& lastDevices = lastQueriedConnectedDevices.get();

            for (auto& d : lastDevices)
                if (d.isDefault)
                    return { d.name, d.containerID };

            return {};
        }

        WinRTMIDIDeviceInfo getWinRTDeviceInfoForDevice (const String& deviceIdentifier)
        {
            // Seem to need this in order for lastQueriedConnectedDevices to contain the correct entries...
            getAvailableDevices();

            const auto& ds = lastQueriedConnectedDevices.get();
            const auto it = std::find_if(ds.begin(), ds.end(), [&](const auto& d) { return d.containerID == deviceIdentifier; });

            return it != ds.end() ? *it : WinRTMIDIDeviceInfo{};
        }

        const BLEDeviceWatcher& bleDeviceWatcher;

        Array<WinRTMIDIDeviceInfo> connectedDevices;
        CriticalSection deviceChanges;
        ThreadLocalValue<Array<WinRTMIDIDeviceInfo>> lastQueriedConnectedDevices;

        winrt_wrap::DeviceWatcher watcher;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MidiIODeviceWatcher);
    };

    //==============================================================================
    template <typename PortType>
    class WinRTIOWrapper   : private BLEDeviceWatcher::Listener
    {
    public:
        WinRTIOWrapper (BLEDeviceWatcher& bleWatcher,
                        MidiIODeviceWatcher& midiDeviceWatcher,
                        const String& deviceIdentifier)
            : bleDeviceWatcher (bleWatcher),
              midiPort(nullptr)
        {
            {
                const ScopedLock lock (midiDeviceWatcher.deviceChanges);
                deviceInfo = midiDeviceWatcher.getWinRTDeviceInfoForDevice (deviceIdentifier);
            }

            if (deviceInfo.deviceID.isEmpty())
                throw std::runtime_error ("Invalid device index");

            JUCE_WINRT_MIDI_LOG ("Creating JUCE MIDI IO: " << deviceInfo.deviceID);

            if (deviceInfo.containerID.isNotEmpty())
            {
                bleDeviceWatcher.addListener (this);

                const ScopedLock lock (bleDeviceWatcher.deviceChanges);

                // This is just a std::find_if, but for some reason HashMap::Iterator isn't assignable, and it doesn't compile...
                for (const auto& d : bleDeviceWatcher.devices)
                {
                    if (d.containerID == deviceInfo.containerID)
                    {
                        isBLEDevice = true;
                        break;
                    }
                }
            }
        }

        virtual ~WinRTIOWrapper()
        {
            if (midiPort != nullptr)
                midiPort.Close();

            midiPort = nullptr;

            bleDeviceWatcher.removeListener (this);

            disconnect();
        }

        //==============================================================================
        virtual void disconnect()
        {
            JUCE_WINRT_MIDI_LOG("Disconnect MIDI port: " << deviceInfo.deviceID << " " << deviceInfo.name) ;

            if (midiPort != nullptr && isBLEDevice)
            {
                DBG("MidiPort Close");
                midiPort.Close();
            }

            midiPort = nullptr;

            JUCE_WINRT_MIDI_LOG("Closed successfully");
        }

    private:
        //==============================================================================
        void bleDeviceAdded (const String& containerID) override
        {
            if (containerID == deviceInfo.containerID)
                isBLEDevice = true;
        }

        void bleDeviceDisconnected (const String& containerID) override
        {
            if (containerID == deviceInfo.containerID)
            {
                JUCE_WINRT_MIDI_LOG ("Disconnecting MIDI port from BLE disconnection: " << deviceInfo.deviceID
                                     << " " << deviceInfo.containerID << " " << deviceInfo.name);
                disconnect();
            }
        }

    protected:
        //==============================================================================
        BLEDeviceWatcher& bleDeviceWatcher;
        WinRTMIDIDeviceInfo deviceInfo;
        bool isBLEDevice = false;

        PortType midiPort;
    };


    struct WinRTInputWrapper final  : public InputWrapper,
                                      private WinRTIOWrapper<winrt_wrap::MidiInPort>

    {
        WinRTInputWrapper (WinRTMidiService& service, MidiInput& input, const String& deviceIdentifier, MidiInputCallback& cb)
            : WinRTIOWrapper <winrt_wrap::MidiInPort> (*service.bleDeviceWatcher, *service.inputDeviceWatcher, deviceIdentifier),
              inputDevice (input),
              callback (cb)
        {
            {
                JUCE_WINRT_MIDI_LOG("Opening MIDI IO: " << deviceInfo.deviceID);

                // TODO: How can we use the cppwinrt .wait_for(5s) method to avoid blocking indefinitely?
                const auto op = winrt_wrap::MidiInPort::FromIdAsync(winrt::to_hstring(deviceInfo.deviceID.toStdString()));

                constexpr auto TimeoutMs = 2000;
                const auto start = Time::getMillisecondCounterHiRes();
                auto now = start;

                while (op.Status() == winrt_wrap::AsyncStatus::Started && (now - start) <= TimeoutMs)
                {
                    Thread::sleep(50);
                    now = Time::getMillisecondCounterHiRes();
                }

                if (op.Status() == winrt_wrap::AsyncStatus::Completed)
                    midiPort = op.GetResults();
            }

            if (midiPort == nullptr)
            {
                JUCE_WINRT_MIDI_LOG("Timed out waiting for midi input port creation " + deviceInfo.deviceID);

                throw std::runtime_error("Timed out waiting for midi input port creation");
            }

            startTime = Time::getMillisecondCounterHiRes();

            midiPort.MessageReceived([wr = WeakReference(this)](const auto&, const winrt_wrap::MidiMessageReceivedEventArgs& args)
            {
                DBG("midi_callback, thread: " << String(GetCurrentThreadId()));

                if (auto* p = wr.get())
                    p->midiInMessageReceived(args);
            });
        }

        ~WinRTInputWrapper() = default;

        //==============================================================================
        void start() override
        {
            if (! isStarted)
            {
                concatenator.reset();
                isStarted = true;
            }
        }

        void stop() override
        {
            if (isStarted)
            {
                isStarted = false;
                concatenator.reset();
            }
        }

        String getDeviceIdentifier() override    { return deviceInfo.containerID; }
        String getDeviceName() override          { return deviceInfo.name; }

        //==============================================================================
        void disconnect() override
        {
            stop();

            WinRTIOWrapper<winrt_wrap::MidiInPort>::disconnect();
        }

        //==============================================================================
        void midiInMessageReceived (const winrt_wrap::MidiMessageReceivedEventArgs& args)
        {
            if (!isStarted)
                return;

            const auto message = args.Message();
            const auto buffer = message.RawData();

            const uint8_t* bufferData = buffer.data();
            const uint32_t numBytes = buffer.Length();
            const auto timespan = message.Timestamp();

            concatenator.pushMidiData (bufferData, numBytes,
                                       convertTimeStamp (timespan.count()),
                                       &inputDevice, callback);
        }

        double convertTimeStamp (int64 timestamp)
        {
            auto millisecondsSinceStart = static_cast<double> (timestamp) / 10000.0;
            auto t = startTime + millisecondsSinceStart;
            auto now = Time::getMillisecondCounterHiRes();

            if (t > now)
            {
                if (t > now + 2.0)
                    startTime -= 1.0;

                t = now;
            }

            return t * 0.001;
        }

        //==============================================================================
        MidiInput& inputDevice;
        MidiInputCallback& callback;

        MidiDataConcatenator concatenator { 4096 };

        double startTime = 0;
        bool isStarted = false;

        JUCE_DECLARE_WEAK_REFERENCEABLE(WinRTInputWrapper);
        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WinRTInputWrapper);
    };


    //==============================================================================
    struct WinRTOutputWrapper final  : public OutputWrapper,
                                       private WinRTIOWrapper <winrt_wrap::IMidiOutPort>
    {
        WinRTOutputWrapper (WinRTMidiService& service, const String& deviceIdentifier)
            : WinRTIOWrapper <winrt_wrap::IMidiOutPort> (*service.bleDeviceWatcher, *service.outputDeviceWatcher, deviceIdentifier),
              buffer(65536)
        {
            {
                // TODO: Copy-pasta from WinRTInputWrapper...
                // TODO: How can we use the cppwinrt .wait_for(5s) method to avoid blocking indefinitely?
                const auto op = winrt_wrap::MidiOutPort::FromIdAsync(winrt::to_hstring(deviceIdentifier.toStdString()));

                constexpr auto TimeoutMs = 2000;
                const auto start = Time::getMillisecondCounterHiRes();
                auto now = start;

                while (op.Status() == winrt_wrap::AsyncStatus::Started && (now - start) <= TimeoutMs);
                {
                    Thread::sleep(50);
                    now = Time::getMillisecondCounterHiRes();
                }

                if (op.Status() == winrt_wrap::AsyncStatus::Completed)
                    midiPort = op.GetResults();
            }

            if (midiPort == nullptr)
                throw std::runtime_error ("Timed out waiting for midi output port creation");

            JUCE_WINRT_MIDI_LOG("Port open success " << String(winrt::to_string(midiPort.DeviceId())));
        }

        //==============================================================================
        void sendMessageNow (const MidiMessage& message) override
        {
            if (midiPort == nullptr)
                return;

            const auto numBytes = message.getRawDataSize();
            buffer.Length(numBytes);

            jassert(buffer.Length() == numBytes);

            memcpy_s (buffer.data(), numBytes, message.getRawData(), numBytes);
            midiPort.SendBuffer (buffer);
        }

        String getDeviceIdentifier() override    { return deviceInfo.containerID; }
        String getDeviceName() override          { return deviceInfo.name; }

        //==============================================================================
        winrt_wrap::Buffer buffer;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WinRTOutputWrapper);
    };

    std::unique_ptr<MidiIODeviceWatcher>  inputDeviceWatcher, outputDeviceWatcher;
    std::unique_ptr<BLEDeviceWatcher> bleDeviceWatcher;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (WinRTMidiService)
};

#endif   // JUCE_USE_WINRT_MIDI

//==============================================================================
//==============================================================================
#if ! JUCE_MINGW
 extern RTL_OSVERSIONINFOW getWindowsVersionInfo();
#endif

struct MidiService :  public DeletedAtShutdown
{
    MidiService()
    {
      #if JUCE_USE_WINRT_MIDI && ! JUCE_MINGW
       #if ! JUCE_FORCE_WINRT_MIDI
        auto windowsVersionInfo = getWindowsVersionInfo();
        if (windowsVersionInfo.dwMajorVersion >= 10 && windowsVersionInfo.dwBuildNumber >= 17763)
       #endif
        {
            try
            {
                internal.reset (new WinRTMidiService());
                return;
            }
            catch (std::runtime_error&) {}
        }
      #endif

        internal.reset (new Win32MidiService());
    }

    ~MidiService()
    {
        clearSingletonInstance();
    }

    static MidiServiceType& getService()
    {
        jassert (getInstance()->internal != nullptr);
        return *getInstance()->internal.get();
    }

    JUCE_DECLARE_SINGLETON (MidiService, false)

private:
    std::unique_ptr<MidiServiceType> internal;
};

JUCE_IMPLEMENT_SINGLETON (MidiService)

//==============================================================================
static int findDefaultDeviceIndex (const Array<MidiDeviceInfo>& available, const MidiDeviceInfo& defaultDevice)
{
    for (int i = 0; i < available.size(); ++i)
        if (available.getUnchecked (i) == defaultDevice)
            return i;

    return 0;
}

Array<MidiDeviceInfo> MidiInput::getAvailableDevices()
{
    return MidiService::getService().getAvailableDevices (true);
}

MidiDeviceInfo MidiInput::getDefaultDevice()
{
    return MidiService::getService().getDefaultDevice (true);
}

std::unique_ptr<MidiInput> MidiInput::openDevice (const String& deviceIdentifier, MidiInputCallback* callback)
{
    if (deviceIdentifier.isEmpty() || callback == nullptr)
        return {};

    std::unique_ptr<MidiInput> in (new MidiInput ({}, deviceIdentifier));
    std::unique_ptr<MidiServiceType::InputWrapper> wrapper;

    try
    {
        wrapper.reset (MidiService::getService().createInputWrapper (*in, deviceIdentifier, *callback));
    }
    catch (std::runtime_error&)
    {
        return {};
    }

    in->setName (wrapper->getDeviceName());
    in->internal = wrapper.release();

    return in;
}

StringArray MidiInput::getDevices()
{
    StringArray deviceNames;

    for (auto& d : getAvailableDevices())
        deviceNames.add (d.name);

    return deviceNames;
}

int MidiInput::getDefaultDeviceIndex()
{
    return findDefaultDeviceIndex (getAvailableDevices(), getDefaultDevice());
}

std::unique_ptr<MidiInput> MidiInput::openDevice (int index, MidiInputCallback* callback)
{
    return openDevice (getAvailableDevices()[index].identifier, callback);
}

MidiInput::MidiInput (const String& deviceName, const String& deviceIdentifier)
    : deviceInfo (deviceName, deviceIdentifier)
{
}

MidiInput::~MidiInput()
{
    delete static_cast<MidiServiceType::InputWrapper*> (internal);
}

void MidiInput::start()   { static_cast<MidiServiceType::InputWrapper*> (internal)->start(); }
void MidiInput::stop()    { static_cast<MidiServiceType::InputWrapper*> (internal)->stop(); }

//==============================================================================
Array<MidiDeviceInfo> MidiOutput::getAvailableDevices()
{
    return MidiService::getService().getAvailableDevices (false);
}

MidiDeviceInfo MidiOutput::getDefaultDevice()
{
    return MidiService::getService().getDefaultDevice (false);
}

std::unique_ptr<MidiOutput> MidiOutput::openDevice (const String& deviceIdentifier)
{
    if (deviceIdentifier.isEmpty())
        return {};

    std::unique_ptr<MidiServiceType::OutputWrapper> wrapper;

    try
    {
        wrapper.reset (MidiService::getService().createOutputWrapper (deviceIdentifier));
    }
    catch (std::runtime_error&)
    {
        return {};
    }

    std::unique_ptr<MidiOutput> out;
    out.reset (new MidiOutput (wrapper->getDeviceName(), deviceIdentifier));

    out->internal = wrapper.release();

    return out;
}

StringArray MidiOutput::getDevices()
{
    StringArray deviceNames;

    for (auto& d : getAvailableDevices())
        deviceNames.add (d.name);

    return deviceNames;
}

int MidiOutput::getDefaultDeviceIndex()
{
    return findDefaultDeviceIndex (getAvailableDevices(), getDefaultDevice());
}

std::unique_ptr<MidiOutput> MidiOutput::openDevice (int index)
{
    return openDevice (getAvailableDevices()[index].identifier);
}

MidiOutput::~MidiOutput()
{
    stopBackgroundThread();
    delete static_cast<MidiServiceType::OutputWrapper*> (internal);
}

void MidiOutput::sendMessageNow (const MidiMessage& message)
{
    static_cast<MidiServiceType::OutputWrapper*> (internal)->sendMessageNow (message);
}

} // namespace juce
                                                                                                                                                          