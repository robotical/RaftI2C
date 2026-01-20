/////////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// Device Ident Manager
//
// Rob Dobson 2024
//
/////////////////////////////////////////////////////////////////////////////////////////////////////////////////

#pragma once

#include "RaftBusDevicesIF.h"
#include "DeviceTypeRecord.h"
#include "BusStatusMgr.h"
#include "DeviceStatus.h"
#include "OfflineDataStoreNVS.h"
#include "RaftJson.h"
#include <vector>
#include <list>
#include <map>
#include <string>
#include <set>

class DeviceIdentMgr : public RaftBusDevicesIF
{
public:
    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Constructor
    /// @param busStatusMgr bus status manager
    /// @param busReqSyncFn bus synchronous access request function
    DeviceIdentMgr(BusStatusMgr& busStatusMgr, BusReqSyncFn busReqSyncFn);

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Setup
    /// @param config configuration
    void setup(const RaftJsonIF& config);

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Loop (periodic service)
    void loop();

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Get list of device addresses attached to the bus
    /// @param pAddrList pointer to array to receive addresses
    /// @param onlyAddressesWithIdentPollResponses true to only return addresses with ident poll responses    
    virtual void getDeviceAddresses(std::vector<BusElemAddrType>& addresses, bool onlyAddressesWithIdentPollResponses) const override final;

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Get device type information by address
    /// @param address address of device to get information for
    /// @param includePlugAndPlayInfo true to include plug and play information
    /// @return JSON string
    virtual String getDevTypeInfoJsonByAddr(BusElemAddrType address, bool includePlugAndPlayInfo) const override final;

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Get device type information by device type name
    /// @param deviceType device type name
    /// @param includePlugAndPlayInfo true to include plug and play information
    /// @return JSON string
    virtual String getDevTypeInfoJsonByTypeName(const String& deviceType, bool includePlugAndPlayInfo) const override final;

    ///////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Get device type info JSON by device type index
    /// @param deviceTypeIdx device type index
    /// @param includePlugAndPlayInfo include plug and play info
    /// @return JSON string
    virtual String getDevTypeInfoJsonByTypeIdx(uint16_t deviceTypeIdx, bool includePlugAndPlayInfo) const override final;
     
    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Get queued device data in JSON format
    /// @return JSON string
    virtual String getQueuedDeviceDataJson(uint32_t maxResponsesToReturn = 0, uint32_t* pRemaining = nullptr) const override final;

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Get queued device data in binary format
    /// @param connMode connection mode (inc bus number)
    /// @return Binary data vector
    virtual std::vector<uint8_t> getQueuedDeviceDataBinary(uint32_t connMode, uint32_t maxResponsesToReturn = 0,
                uint32_t* pRemaining = nullptr) const override final;

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Get decoded poll responses
    /// @param address address of device to get data from
    /// @param pStructOut pointer to structure (or array of structures) to receive decoded data
    /// @param structOutSize size of structure (in bytes) to receive decoded data
    /// @param maxRecCount maximum number of records to decode
    /// @param decodeState decode state for this device
    /// @return number of records decoded
    /// @note the pStructOut should generally point to structures of the correct type for the device data and the
    ///       decodeState should be maintained between calls for the same device
    virtual uint32_t getDecodedPollResponses(BusElemAddrType address, 
                    void* pStructOut, uint32_t structOutSize, 
                    uint16_t maxRecCount, RaftBusDeviceDecodeState& decodeState) const override final;

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Handle poll results
    /// @param timeNowUs time in us (passed in to aid testing)
    /// @param address address
    /// @param pollResultData poll result data
    /// @param pPollInfo pointer to device polling info (maybe nullptr) 
    /// @return true if result stored
    virtual bool handlePollResult(uint64_t timeNowUs, BusElemAddrType address, 
                            const std::vector<uint8_t>& pollResultData, const DevicePollingInfo* pPollInfo) override final
    {
        return _busStatusMgr.handlePollResult(0, timeNowUs, address, pollResultData, pPollInfo, 0);
    }

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Register for device data notifications
    /// @param addrAndSlot address
    /// @param dataChangeCB Callback for data change
    /// @param minTimeBetweenReportsMs Minimum time between reports (ms)
    /// @param pCallbackInfo Callback info (passed to the callback)
    virtual void registerForDeviceData(BusElemAddrType address, RaftDeviceDataChangeCB dataChangeCB, 
                uint32_t minTimeBetweenReportsMs, const void* pCallbackInfo) override final
    {
        _busStatusMgr.registerForDeviceData(address, dataChangeCB, minTimeBetweenReportsMs, pCallbackInfo);
    }

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Get debug JSON
    /// @return JSON string
    virtual String getDebugJSON(bool includeBraces) const override final;

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Offline stats passthrough
    virtual OfflineDataStats getOfflineStats(BusElemAddrType address) const override final
    {
        return _busStatusMgr.getOfflineStats(address);
    }

    /// @brief Override max per publish for offline backlog
    void setOfflineMaxPerPublishOverride(uint32_t maxPerPublish) override;

    /// @brief Update selection of addresses/types allowed for draining offline buffers
    void setOfflineDrainSelection(const std::vector<BusElemAddrType>& addresses, const std::vector<std::string>& typeNames,
                bool drainOnlySelected) override;

    /// @brief Pause/resume buffering for addresses
    void setOfflineBufferPaused(const std::vector<BusElemAddrType>& addresses, bool paused) override;

    /// @brief Estimate offline allocation bytes for addresses without applying changes
    bool estimateOfflineAllocations(const std::vector<BusElemAddrType>& addresses,
                std::map<BusElemAddrType, EstAllocInfo>& allocBytesOut) const override;

    /// @brief Pause/resume draining for addresses
    void setOfflineDrainPaused(const std::vector<BusElemAddrType>& addresses, bool paused) override;

    /// @brief Pause/resume draining due to link availability
    void setOfflineDrainLinkPaused(bool paused) override;

    /// @brief Reset offline buffers for addresses
    void resetOfflineBuffers(const std::vector<BusElemAddrType>& addresses) override;

    /// @brief Snapshot control state for diagnostics
    void getOfflineControlSnapshot(std::set<BusElemAddrType>& bufferPaused, std::set<BusElemAddrType>& drainPaused,
                std::set<BusElemAddrType>& drainSelectedAddrs, std::set<std::string>& drainSelectedTypes,
                bool& drainOnlySelected, uint32_t& maxPerPublishOverride,
                bool& globalBufferPaused, bool& globalDrainPaused,
                std::map<BusElemAddrType, uint32_t>& rateOverridesUs) const override;

    /// @brief Peek at offline data without consuming
    String peekOfflineDataJson(const std::vector<BusElemAddrType>& addresses,
                uint32_t startIdx, uint32_t maxResponsesToReturn, uint32_t maxBytes,
                uint32_t& totalRemaining) override;

    /// @brief Apply a rate override (ms) while buffering
    bool applyOfflineRateOverride(const std::vector<BusElemAddrType>& addresses, uint32_t pollRateMs) override;

    /// @brief Clear any rate overrides and restore defaults
    bool clearOfflineRateOverride(const std::vector<BusElemAddrType>& addresses) override;

    /// @brief Rebalance offline buffer depths across devices
    bool rebalanceOfflineBuffers(const std::vector<BusElemAddrType>& addresses) override;

    /// @brief Get device type name for an address
    bool getDeviceTypeName(BusElemAddrType address, std::string& typeName) const override;

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Identify device
    /// @param 
    /// @param deviceStatus (out) device status
    void identifyDevice(BusElemAddrType address, DeviceStatus& deviceStatus);

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Check device type match (communicates with the device to check its type)
    /// @param address address
    /// @param pDevTypeRec device type record
    /// @return true if device type matches
    bool checkDeviceTypeMatch(BusElemAddrType address, const DeviceTypeRecord* pDevTypeRec);

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Process device initialisation
    /// @param address address
    /// @param pDevTypeRec device type record
    /// @return true if device initialisation was successful
    bool processDeviceInit(BusElemAddrType address, const DeviceTypeRecord* pDevTypeRec);

private:
    // Device indentification enabled
    bool _isEnabled = false;

    // Bus status
    BusStatusMgr& _busStatusMgr;

    // Bus request function
    BusReqSyncFn _busReqSyncFn = nullptr;

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Format device status to JSON
    /// @param address address
    /// @param isOnline true if device is online
    /// @param deviceTypeIndex index of device type
    /// @param devicePollResponseData poll response data
    /// @param responseSize size of poll response data
    /// @return JSON string
    String deviceStatusToJson(BusElemAddrType address, bool isOnline, uint16_t deviceTypeIndex, 
                    const std::vector<uint8_t>& devicePollResponseData, uint32_t responseSize,
                    bool isBacklog, uint32_t remainingCount, const OfflineDataMeta* pFirstMeta,
                    const OfflineDataStats& stats) const;

    /////////////////////////////////////////////////////////////////////////////////////////////////////////////////
    /// @brief Decode one or more poll responses for a device
    /// @param deviceTypeIndex index of device type
    /// @param pPollBuf buffer containing poll responses
    /// @param pollBufLen length of poll response buffer
    /// @param pStructOut pointer to structure (or array of structures) to receive decoded data
    /// @param structOutSize size of structure (in bytes) to receive decoded data
    /// @param maxRecCount maximum number of records to decode
    /// @return number of records decoded
    uint32_t decodePollResponses(uint16_t deviceTypeIndex, 
                    const uint8_t* pPollBuf, uint32_t pollBufLen, 
                    void* pStructOut, uint32_t structOutSize, 
                    uint16_t maxRecCount, RaftBusDeviceDecodeState& decodeState) const;

    struct OfflineBufferPolicy
    {
        uint32_t perDeviceMaxBytes = 0;
        uint32_t globalMaxBytes = 0;
        uint32_t defaultWindowMs = 0;
        uint32_t minSamples = 1;
        uint32_t maxPerPublish = 0;
        uint32_t memUsePermille = 750;
        std::map<std::string, uint32_t> perDeviceWindowMs;
    };

    struct OfflineNvsConfig
    {
        bool enabled = false;
        uint32_t flushIntervalMs = 10000;
        bool importOnBoot = true;
    };

    struct OfflineNvsState
    {
        OfflineDataStoreNVS store;
        bool imported = false;
        bool hasFlushedSeq = false;
        uint32_t lastFlushedSeq = 0;
        uint32_t lastFlushMs = 0;
        uint32_t ramMaxEntries = 0;
        uint32_t payloadSize = 0;
        uint32_t timestampBytes = 0;
        uint32_t timestampResolutionUs = 0;
    };

    uint32_t calcOfflineDepth(const DeviceTypeRecord& devTypeRec, const DevicePollingInfo& pollInfo) const;
    void parseOfflineConfig(const RaftJsonIF& config);
    void setOfflineStatsRemaining(uint32_t remaining, uint32_t* pRemaining) const;
    uint32_t applyGlobalOfflineLimit(const DevicePollingInfo& pollInfo, uint32_t requestedDepth) const;
    uint32_t getPerDevicePublishLimit(uint32_t maxResponsesToReturn) const;
    bool isOfflineDrainAllowed(BusElemAddrType address, uint16_t deviceTypeIndex) const;
    void applyOfflineControlsToDevice(BusElemAddrType address, DeviceStatus& deviceStatus);
    void applyOfflineControlToExisting();
    void applyOfflineControlToAddress(BusElemAddrType address, uint16_t deviceTypeIdx);
    void computeOfflineControlFlags(BusElemAddrType address, const std::string& devTypeName,
                bool& bufferPaused, bool& drainPaused, bool& restrictToSelection) const;
    uint32_t computeDepthForAddress(BusElemAddrType address, const DevicePollingInfo& pollInfo) const;
    bool applyRateOverrideToAddress(BusElemAddrType address, uint32_t pollRateMs, bool recordOriginal);
    bool clearRateOverrideForAddress(BusElemAddrType address);

    void configureOfflineNvsState(BusElemAddrType address, const DevicePollingInfo& pollInfo, uint32_t maxEntries);
    void flushOfflineNvs(uint32_t nowMs);
    void clearOfflineNvsState(const std::vector<BusElemAddrType>& addresses);
    void ensureOfflineNvsForPeek(const std::vector<BusElemAddrType>& addresses);

    // Debug
    static constexpr const char* MODULE_PREFIX = "RaftDevIdentMgr";

    OfflineBufferPolicy _offlinePolicy;
    OfflineNvsConfig _offlineNvsConfig;
    std::map<BusElemAddrType, OfflineNvsState> _offlineNvsStates;
    uint32_t _maxPerPublishOverride = 0;
    bool _drainOnlySelected = false;
    std::set<BusElemAddrType> _drainSelectedAddrs;
    std::set<std::string> _drainSelectedTypes;
    std::set<BusElemAddrType> _bufferPausedAddrs;
    std::set<BusElemAddrType> _drainPausedAddrs;
    bool _globalBufferPaused = true;
    bool _globalDrainPaused = true;
    bool _linkDrainPaused = false;
    SemaphoreHandle_t _offlineCtrlMutex = nullptr;
    std::map<BusElemAddrType, uint32_t> _rateOverridesUs;
    std::map<BusElemAddrType, uint32_t> _rateOverrideOriginalUs;
};
