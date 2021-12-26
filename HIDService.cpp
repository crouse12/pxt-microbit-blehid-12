
#include "MicroBitConfig.h"

#if CONFIG_ENABLED(DEVICE_BLE)

#include "HIDService.h"
#include "ble_srv_common.h"

#include "pxt.h"
#include "MicroBit.h"

// Advertising includes
#include "ble.h"
#include "ble_hci.h"
#include "ble_srv_common.h"
#include "ble_advdata.h"
#include "ble_conn_params.h"
#include "ble_dis.h"

#include "debug.h"


using namespace pxt;

// Initialize static members

const uint16_t HIDService::hidService = 0x1812; 

const uint16_t HIDService::charUUID[mbbs_cIdxCOUNT] = { 
  // 0x8A4E,  //  ProtocolMode
  // 0x8A4A,  //  HIDInfo
  // 0x8A4B,  //  Report Map
  // 0x8A4D,  //  Report 0
  // 0x8A4D,  //  Report 1
  // 0x8A4D,  //  Report 2
  // 0x8A4D,  //  Report 3
  0x2A4E,  //  ProtocolMode
  0x2A4A,  //  HIDInfo
  0x2A4B,  //  Report Map
  0x2A4D,  //  Report 0
  0x2A4D,  //  Report 1
  0x2A4D,  //  Report 2
  0x2A4D,  //  Report 3
};

uint16_t HIDService::HIDInfo[2] = { 
  0x0111,
  0x0002
};

const int HIDService::EVT_STATUS = 1;  // Event for connect / disconnect


HIDService *HIDService::service = NULL; // Singleton reference to the service

/**
 */
HIDService *HIDService::getInstance()
{
    if (service == NULL)
    {
        service = new HIDService();
    }
    return service;
}

/** 
 * Constructor.
 * Create a representation of the Bluetooth SIG Battery Service
 * @param _ble The instance of a BLE device that we're running on.
 */
HIDService::HIDService() :
  protocolMode(0x01),  // Report Protocol
  numReporters(0), 
  reportMapUsed(0)

{
  // Initialize all report data 
    memset(reporters, 0, sizeof(HIDReporter*)*numReportsMax);

    DEBUG("HID Serv starting\n");
    // Update advertisements 
    advertiseHID();

    // Register the base UUID and create the service.
    bs_uuid_type = BLE_UUID_TYPE_BLE;  // Set the UUID type to 0x01, which should be Bluetooth SIG ID
    CreateService( hidService );

    // Create the data structures that represent each of our characteristics in Soft Device.
    // iOS needs protocol mode characteristic for keyboard support
    CreateCharacteristic( mbbs_cIdxProtocolMode, charUUID[ mbbs_cIdxProtocolMode ],
                        (uint8_t *)&protocolMode,
                        sizeof(protocolMode), sizeof(protocolMode),
                        microbit_propREAD | microbit_propWRITE_WITHOUT ); 

    CreateCharacteristic( mbbs_cIdxHIDInfo, charUUID[ mbbs_cIdxHIDInfo ],
                        (uint8_t *)HIDInfo,
                        sizeof(HIDInfo), sizeof(HIDInfo),
                        microbit_propREAD );
 
    memset(reportMap, 0, reportMapMaxSize);
    CreateCharacteristic( mbbs_cIdxReportMap, charUUID[ mbbs_cIdxReportMap ],
                        (uint8_t *)reportMap,
                        0, reportMapMaxSize,
                        microbit_propREAD | microbit_propREADAUTH );

    for(int i=mbbs_cIdxReport1, idx=0; i<mbbs_cIdxCOUNT;i++, idx++) {
      memset(reports[idx], 0, reportMaxSize);
      CreateCharacteristic(i, charUUID[i],
                          reports[idx],
                          0, reportMaxSize,
                          microbit_propREAD  | microbit_propNOTIFY | microbit_propREADAUTH);
      // Must have report discriptor for OS detection
      // NOTE: Assuming INPUT reports
      // Report indices are 1-based and in order of addition
      addReportDescriptor(charHandles(i)->value, i-mbbs_cIdxReport1+1, 1 /* Input report */);
    } 
}

void HIDService::addReporter(HIDReporter *reporter) {
  // See if there's room for another
  uint16_t mapSize = reporter->reportMapSize;
  DEBUG("HID Adding reporter %s (%d)\n", reporter->name, mapSize);
  if(numReporters>=numReportsMax || reportMapUsed+mapSize+2>=reportMapMaxSize) {
    // TO DO: Throw exception / halt REVIEW
    DEBUG("ERROR: No more space for reports");
    target_panic(PANIC_INVALID_ARGUMENT); 
  }

  // Update reporter data for reporting...
  reporter->reportID = numReporters+1;
  reporter->reportIndex = numReporters+mbbs_cIdxReport1;

  // Add the Report Map
  memcpy(reportMap+reportMapUsed, reporter->reportMap, mapSize);
  reportMap[reportMapUsed+reporter->reportIDOffset] = reporter->reportID;
  reportMapUsed += mapSize;

  DEBUG("char index %d   report id %d\n", reporter->reportIndex, reporter->reportID);

  // Update the list of reporters
  reporters[numReporters++] = reporter;
}

/**
  * Invoked when BLE connects.
  */
void HIDService::onConnect( const microbit_ble_evt_t *p_ble_evt)
{
    DEBUG("HID onConnect\n");
}

/**
  * Invoked when BLE disconnects.
  */
void HIDService::onDisconnect( const microbit_ble_evt_t *p_ble_evt)
{
    DEBUG("HID onDisconnect\n");
    for(int i=0;i<numReporters;i++) {
      reporters[i]->setEnabled(false);
    }
}

void HIDService::onDataRead( microbit_onDataRead_t *params) {
      DEBUG("HID onDataRead\n");
      debugAttribute(params->handle);
      microbit_charattr_t type;
      int index = charHandleToIdx(params->handle, &type);
      int offset = params->offset;
      if(index == mbbs_cIdxReportMap && type == microbit_charattrVALUE) {
        params->data = &(reportMap[offset]);
        params->length = max(reportMapUsed-offset,0);  // Remaining data
      }

}

/**
  * Callback. Invoked when any of our attributes are written via BLE.
  */
void HIDService::onDataWritten( const microbit_ble_evt_write_t *params)
{
    DEBUG("HID onDataWritten\n");
    debugAttribute(params->handle);

    microbit_charattr_t type;
    int index = charHandleToIdx(params->handle, &type);

    if(index>=mbbs_cIdxReport1 && index<=mbbs_cIdxCOUNT && type == microbit_charattrCCCD) {
      DEBUG("HID Report CCCD Changed\n");
      bool status = params->len>0 && params->data[0] ? true : false;
      int reporterIdx = index-mbbs_cIdxReport1;
      HIDReporter *theReporter = reporters[reporterIdx];
      if(theReporter!=NULL) {
        theReporter->setEnabled(status);
      }
  } 
}

/**
 * 
 * 
*/
void HIDService::addReportDescriptor(uint16_t value_handle, uint8_t reportID, uint8_t reportType) {
    ble_gatts_attr_md_t attr_md;
    ble_gatts_attr_t    attr_char_value;
    ble_uuid_t          ble_uuid;
    ble_srv_report_ref_t rep_ref;

    memset((uint8_t *)&rep_ref, 0, sizeof(rep_ref));
    rep_ref.report_id = reportID;
    rep_ref.report_type = reportType; // Input report

    // Referenced from https://github.com/xueliu/nRF52/blob/master/nRF52_SDK_0.9.1_3639cc9/components/ble/ble_services/ble_hids/ble_hids.c
    uint8_t             encoded_rep_ref[BLE_SRV_ENCODED_REPORT_REF_LEN];
    memset(&attr_md, 0, sizeof(attr_md));
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&attr_md.read_perm); //rep_ref_attr_md.read_perm;
    BLE_GAP_CONN_SEC_MODE_SET_ENC_NO_MITM(&attr_md.write_perm); //rep_ref_attr_md.write_perm;
    attr_md.vloc       = BLE_GATTS_VLOC_STACK;
    attr_md.rd_auth    = 0;
    attr_md.wr_auth    = 0;
    attr_md.vlen       = 0;
    
    memset(&attr_char_value, 0, sizeof(attr_char_value));
    BLE_UUID_BLE_ASSIGN(ble_uuid, BLE_UUID_REPORT_REF_DESCR);
    attr_char_value.p_uuid    = &ble_uuid; 
    attr_char_value.p_attr_md = &attr_md;
    attr_char_value.init_len  = ble_srv_report_ref_encode(encoded_rep_ref, &rep_ref); 

    attr_char_value.init_offs = 0;
    attr_char_value.max_len   = attr_char_value.init_len;
    attr_char_value.p_value   = encoded_rep_ref;
    uint16_t reportCCCDHandle;
    sd_ble_gatts_descriptor_add(value_handle, 
                                  &attr_char_value,
                                  &reportCCCDHandle); 
}

void HIDService::advertiseHID() {
        // Stop any active advertising
        uBit.bleManager.stopAdvertising();

        // m_advdata _must_ be static / retained!
        static ble_advdata_t m_advdata;
        // m_enc_advdata _must_ be static / retained!
        static uint8_t  m_enc_advdata[BLE_GAP_ADV_SET_DATA_SIZE_MAX];
        static ble_uuid_t uuid;  // UUID Struct
        uint8_t m_adv_handle;

        MICROBIT_DEBUG_DMESG( "configureAdvertising connectable %d, discoverable %d", (int) connectable, (int) discoverable);
        MICROBIT_DEBUG_DMESG( "whitelist %d, interval_ms %d, timeout_seconds %d", (int) whitelist, (int) interval_ms, (int) timeout_seconds);
        uuid.type = BLE_UUID_TYPE_BLE;
        uuid.uuid = 0x1812; // 1812 is HID 
        m_advdata.uuids_complete.uuid_cnt = 1;
        m_advdata.uuids_complete.p_uuids = &uuid;
        m_advdata.include_appearance = true;
        // Name needed to be identified by Android
        m_advdata.name_type = BLE_ADVDATA_FULL_NAME;
        
        // Appearance isn't stricly needed for detection 
        sd_ble_gap_appearance_set(BLE_APPEARANCE_GENERIC_HID );

        // The flags below ensure "pairing mode" so it shows up in Android
        m_advdata.flags = BLE_GAP_ADV_FLAG_BR_EDR_NOT_SUPPORTED | BLE_GAP_ADV_FLAG_LE_GENERAL_DISC_MODE;

        ble_gap_adv_params_t    gap_adv_params;
        memset( &gap_adv_params, 0, sizeof( gap_adv_params));
        gap_adv_params.properties.type  = true /* connectable */
                                        ? BLE_GAP_ADV_TYPE_CONNECTABLE_SCANNABLE_UNDIRECTED
                                        : BLE_GAP_ADV_TYPE_NONCONNECTABLE_SCANNABLE_UNDIRECTED;
        gap_adv_params.interval         = ( 1000 * MICROBIT_BLE_ADVERTISING_INTERVAL/* interval_ms */) / 625;  // 625 us units
        if ( gap_adv_params.interval < BLE_GAP_ADV_INTERVAL_MIN) gap_adv_params.interval = BLE_GAP_ADV_INTERVAL_MIN;
        if ( gap_adv_params.interval > BLE_GAP_ADV_INTERVAL_MAX) gap_adv_params.interval = BLE_GAP_ADV_INTERVAL_MAX;
        gap_adv_params.duration         = MICROBIT_BLE_ADVERTISING_TIMEOUT /* timeout_seconds */ * 100;              //10 ms units
        gap_adv_params.filter_policy    = false /* whitelist */
                                        ? BLE_GAP_ADV_FP_FILTER_BOTH
                                        : BLE_GAP_ADV_FP_ANY;
        gap_adv_params.primary_phy      = BLE_GAP_PHY_1MBPS;
                    
        ble_gap_adv_data_t  gap_adv_data;
        memset( &gap_adv_data, 0, sizeof( gap_adv_data));
        gap_adv_data.adv_data.p_data    = m_enc_advdata;
        gap_adv_data.adv_data.len       = BLE_GAP_ADV_SET_DATA_SIZE_MAX;

        MICROBIT_BLE_ECHK( ble_advdata_encode( &m_advdata, gap_adv_data.adv_data.p_data, &gap_adv_data.adv_data.len));
        MICROBIT_BLE_ECHK( sd_ble_gap_adv_set_configure( &m_adv_handle, &gap_adv_data, &gap_adv_params));

        // Restart advertising
        // WARNING: This will start adv using the static handle in the BLE Manager. 
        // Hopefully the same handle is used as the one returned by sd_ble_gap_adv_set_configure
        uBit.bleManager.advertise();
    }

#ifdef DEBUG_ENABLED
void HIDService::debugAttribute(int handle) {
      microbit_charattr_t type;
      int index = charHandleToIdx(handle, &type);

      const char *typeName;
      switch(type) {
        case microbit_charattrVALUE:
          typeName = "Value";
          break;
        case microbit_charattrDESC:
          typeName = "Desc";
          break;
        case microbit_charattrCCCD:
          typeName = "CCCD";
          break;
        case microbit_charattrSCCD:
          typeName = "SCCD";
          break;
        default:
          typeName = "UNKNOWN";
      }
      if(index<0 || index>=mbbs_cIdxCOUNT) index = 4;
      char const *charNames[] = {"Protocol", "Info", "Map", "Report", "Invalid"};
      if(index>=mbbs_cIdxReport1 && index<mbbs_cIdxCOUNT) 
      {
          int report = index-mbbs_cIdxReport1;
          index=3;
          // TODO: Add in reporter name
          if(reporters[report]) {
            DEBUG("     %s %s (%d : %s)\n", charNames[index], typeName, report, reporters[report]->name);
          } else {
            DEBUG("     %s %s (%d) (BAD INDEX)\n", charNames[index], typeName, report);
          }
      } else {
        DEBUG("     %s %s\n", charNames[index], typeName);
      }
}
#endif

#endif