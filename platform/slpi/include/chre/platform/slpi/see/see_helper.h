/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef CHRE_PLATFORM_SLPI_SEE_SEE_HELPER_H_
#define CHRE_PLATFORM_SLPI_SEE_SEE_HELPER_H_

#include "qmi_client.h"
#include "sns_suid.pb.h"

#include "chre/core/sensor_request.h"
#include "chre/platform/condition_variable.h"
#include "chre/platform/mutex.h"
#include "chre/util/dynamic_vector.h"
#include "chre/util/non_copyable.h"
#include "chre/util/time.h"
#include "chre/util/unique_ptr.h"

namespace chre {

//! The type of SeeHelper indication callback.
typedef void (SeeIndCallback)(SensorType sensorType,
                              UniquePtr<uint8_t>&& eventData);

//! Default timeout for waitForService. Have a longer timeout since there may be
//! external dependencies blocking SEE initialization.
constexpr Nanoseconds kDefaultSeeWaitTimeout = Seconds(5);

//! Default timeout for sendReq response
constexpr Nanoseconds kDefaultSeeRespTimeout = Seconds(1);

//! Default timeout for sendReq indication
constexpr Nanoseconds kDefaultSeeIndTimeout = Seconds(2);

//! Length of the char array to store sensor string attributes.
constexpr size_t kSeeAttrStrValLen = 64;

//! A struct to facilitate getAttributesSync().
struct SeeAttributes {
  char vendor[kSeeAttrStrValLen];
  char name[kSeeAttrStrValLen];
  char type[kSeeAttrStrValLen];
  float maxSampleRate;
  uint8_t streamType;
};

//! A struct to facilitate making sensor request
struct SeeSensorRequest {
  SensorType sensorType;
  bool enable;
  float samplingRateHz;
  uint32_t batchPeriodUs;
};

// TODO(P2-aa0089): Replace QMI with an interface that doesn't introduce big
// image wakeups.

/**
 * A helper class for making requests to Qualcomm's Sensors Execution
 * Environment (SEE) via QMI and waiting for the corresponding indication
 * message if applicable. Not safe to use from multiple threads.
 * Only one synchronous request can be made at a time.
 */
class SeeHelper : public NonCopyable {
 public:
  //! A struct to facilitate mapping between 'SUID + qmiHandle' and SensorType.
  struct SensorInfo {
    sns_std_suid suid;
    SensorType sensorType;
    qmi_client_type qmiHandle;
  };

  /**
   * A synchronous call to discover SUID(s) that supports the specified data
   * type. This API will clear the provided dynamic vector before populating it.
   *
   * @param dataType A data type string, "accel" for example.
   * @param suids A non-null pointer to a list of sensor UIDs that support the
   *              specified data type.
   *
   * @return true if sensor discovery succeeded even if no SUID was found.
   */
  bool findSuidSync(const char *dataType, DynamicVector<sns_std_suid> *suids);

  /**
   * A synchronous call to obtain the attributes of the specified SUID.
   *
   * @param suid The SUID of the sensor
   * @param attr A non-null pointer to the attibutes of the specified SUID that
   *             include sensor vendor, name and max sampling rate, etc.
   *
   * @return true if the attribute was successfully obtained and attr populated.
   */
  bool getAttributesSync(const sns_std_suid& suid, SeeAttributes *attr);

  /**
   * Initializes and waits for the sensor client QMI service to become
   * available. This function must be called first to initialize the object.
   *
   * @param indCb A pointer to the indication callback. This callback will be
   *              invoked to handle pb-decoded message for all async requests.
   * @param timeout The wait timeout in microseconds.
   *
   * @return true if the qmi client was successfully initialized.
   */
  bool init(SeeIndCallback *indCb,
            Microseconds timeout = kDefaultSeeWaitTimeout);

  /**
   * Makes a sensor request to SEE.
   *
   * @param request The sensor request to make.
   *
   * @return true if the QMI request has been successfully made.
   */
  bool makeRequest(const SeeSensorRequest& request);

  /**
   * Wrapper to call qmi_client_release() and clear the registered SUIDs. After
   * this is called, the object is deinitialized until init is called again.
   */
  bool deinit();

  /**
   * Register a SensorType with the SUID of the SEE sensor/driver.
   *
   * Only registered SUIDs will call the indication callback provided in init()
   * with populated CHRE sensor events. Each SUID/SensorType pair can only be
   * registered once. It's illegal to register SensorType::Unknown.
   *
   * If an SUID is registered with a second SensorType, another QMI client may
   * be created to disambiguate the SUID representation.
   *
   * @param sensorType The SensorType to register.
   * @param suid The SUID of the sensor.
   * @param prevRegistered A non-null pointer to a boolean that indicates
   *        whether the SUID/SensorType pair has been previously registered.
   *
   * @return true if the SUID/SensorType pair was successfully registered.
   */
  bool registerSensor(SensorType sensorType, const sns_std_suid& suid,
                      bool *prevRegistered);

 private:
  /**
   * Initializes SEE calibration sensors and makes data request.
   *
   * @return true if cal sensor have been succcessfully initialized.
   */
  bool initCalSensors();

  /**
   * Wrapper to send a QMI request and wait for the indication if it's a
   * synchronous one.
   *
   * Only one request can be pending at a time per instance of SeeHelper.
   *
   * @param qmiHandle The QMI Handle to make QMI requests with.
   * @param suid The SUID of the sensor the request is sent to
   * @param syncData The data struct or container to receive a sync call's data
   * @param syncDataType The data type we are waiting for.
   * @param msgId Message ID of the request to send
   * @param payload A non-null pointer to the pb-encoded message
   * @param payloadLen The length of payload
   * @param batchValid Whether batchPeriodUs is valid and applicable to this
   *                   request
   * @param batchPeriodUs The batch period in microseconds
   * @param waitForIndication Whether to wait for the indication of the
   *                          specified SUID or not.
   * @param timeoutRresp How long to wait for the response before abandoning it
   * @param timeoutInd How long to wait for the indication before abandoning it
   *
   * @return true if the request has been sent and the response/indication it's
   *         waiting for has been successfully received
   */
  bool sendReq(
      const qmi_client_type& qmiHandle, const sns_std_suid& suid,
      void *syncData, const char *syncDataType,
      uint32_t msgId, void *payload, size_t payloadLen,
      bool batchValid, uint32_t batchPeriodUs,
      bool waitForIndication,
      Nanoseconds timeoutResp = kDefaultSeeRespTimeout,
      Nanoseconds timeoutInd = kDefaultSeeIndTimeout);

  /**
   * Handles the payload of a sns_client_report_ind_msg_v01 message.
   */
  void handleSnsClientEventMsg(
      qmi_client_type clientHandle, const void *payload, size_t payloadLen);

  /**
   * Processes a QMI indication callback
   *
   * @see qmi_client_ind_cb
   */
  void handleInd(qmi_client_type client_handle, unsigned int msg_id,
                 const void *ind_buf, unsigned int ind_buf_len);

  /**
   * Extracts "this" from ind_cb_data and calls through to handleInd()
   *
   * @see qmi_client_ind_cb
   */
  static void qmiIndCb(qmi_client_type client_handle, unsigned int msg_id,
                       void *ind_buf, unsigned int ind_buf_len,
                       void *ind_cb_data);

  /**
   * A wrapper to initialize a QMI client.
   *
   * @ see qmi_client_init_instance
   */
  bool waitForService(qmi_client_type *qmiHandle,
                      Microseconds timeout = kDefaultSeeWaitTimeout);

  //! Data struct to store sync APIs data.
  void *mSyncData = nullptr;

  //! Indication callback for sensor data events.
  SeeIndCallback *mIndCb = nullptr;

  ConditionVariable mCond;
  Mutex mMutex;

  //! true if we are waiting on an indication for a sync call.
  bool mWaiting = false;

  //! The SUID whose indication this SeeHelper is waiting for in a sync call.
  sns_std_suid mSyncSuid = sns_suid_sensor_init_zero;

  //! The data type whose indication this SeeHelper is waiting for in
  //! findSuidSync.
  const char *mSyncDataType = nullptr;

  //! The list of QMI handles initiated by SeeHelper.
  DynamicVector<qmi_client_type> mQmiHandles;

  //! The list of SensorTypes registered and their corresponding SUID and
  //! QMI handle.
  DynamicVector<SensorInfo> mSensorInfos;
};

}  // namespace chre

#endif  // CHRE_PLATFORM_SLPI_SEE_SEE_HELPER_H_
