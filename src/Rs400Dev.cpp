/*
**********************************************************************************************************
*                                                                                                       **
* INTEL CONFIDENTIAL                                                                                    **
* Copyright (2018 - 2020) Intel Corporation.                                                            **
* This software and the related documents are Intel copyrighted materials, and your use of them is      **
* governed by the express license under which they were provided to you ("License"). Unless the License **
* provides otherwise, you may not use, modify, copy, publish, distribute, disclose or transmit this     **
* software or the related documents without Intel's prior written permission.                           **
* This software and the related documents are provided as is, with no express or implied warranties,    **
* other than those that are expressly stated in the License.                                            **
*                                                                                                       **
**********************************************************************************************************
*/

#ifndef _WIN32
#include <dirent.h>
#else
#include <process.h>
#endif

#include <iostream>
#include <string>
#include <cstring>
#include "Rs400Dev.h"
#include "librealsense2/rs_advanced_mode.hpp"

using namespace std;
using namespace RsCamera;
using namespace rs2;

Rs400Device::Rs400Device()
{
    m_context = new context();

    m_captureStarted = false;
    m_stopProcessFrame = true;

	m_depthSensor = NULL;
	m_colorSensor = NULL;

	m_bColorEnabled = false;

    for (uint32_t i = 0; i < NELEMS(m_timestamp); i++)
    {
        m_timestamp[i] = 0;
        m_pData[i] = nullptr;
    }

    m_ts = 0x8000000000;

#ifdef _WIN32
    InitializeCriticalSection(&m_mutex);
#else
    if (0 != pthread_mutex_init(&m_mutex, NULL)) throw std::runtime_error("pthread_mutex_init failed");
#endif
}

Rs400Device::~Rs400Device()
{
	MUTEX_UNLOCK(&m_mutex);

#ifdef _WIN32
	DeleteCriticalSection(&m_mutex);
#else
    if (0 != pthread_mutex_destroy(&m_mutex)) throw std::runtime_error("pthread_mutex_destroy failed");
#endif

    m_depthProfiles.clear();
	m_colorProfile.clear();
    delete m_context;
}


camera_info Rs400Device::InitializeCamera()
{    
	camera_info info{};
	auto devices = m_context->query_devices();
    m_device = devices[0];

    if (m_device.is<rs400::advanced_mode>())
    {
		rs400::advanced_mode advanced = m_device.as<rs400::advanced_mode>();
        if (!advanced.is_enabled())
        {
            advanced.toggle_advanced_mode(true);
            //Remove context recreation after libRs fix
            delete m_context;
            m_context = new context();
            devices = m_context->query_devices();
			m_device = devices[0];
        }
    }

	auto sensors = devices[0].query_sensors();
	m_depthSensor = sensors[0];

	info.name = m_device.get_info(rs2_camera_info::RS2_CAMERA_INFO_NAME);
	// filter out non RS400 camera
	if (info.name.find("RealSense") == string::npos || info.name.find("4") == string::npos) return info;

	info.pid = m_device.get_info(rs2_camera_info::RS2_CAMERA_INFO_PRODUCT_ID);
	info.serial = m_device.get_info(rs2_camera_info::RS2_CAMERA_INFO_SERIAL_NUMBER);
	info.fw_ver = m_device.get_info(rs2_camera_info::RS2_CAMERA_INFO_FIRMWARE_VERSION);

	std::vector<uint8_t> RawBuffer =
	{ 0x14, 0, 0xab, 0xcd, 0x10, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

	std::vector<uint8_t> rcvBuf;

	auto debug = m_device.as<debug_protocol>();;
	rcvBuf = debug.send_and_receive_raw_data(RawBuffer);

	if ((rcvBuf[SKU_COMPONENT] & 0x03) == 2)
		info.isWide = true;

	if (rcvBuf[RGB_MODE] & 0x01)
	{
		info.isRGB = true;
		if (sensors.size() > 1)
		{
			m_colorSensor = sensors[1];
		}
	}

    return info;
}

bool Rs400Device::SetMediaMode(int width, int height, int frameRate, int colorWidth, int colorHeight, bool enableColor)
{
    m_bColorEnabled = enableColor;

    m_depthProfiles.clear();
	m_colorProfile.clear();

    //Enable Y16 format for Left
    stream_profile infraredProfile;
    if (!GetProfile(infraredProfile, rs2_stream::RS2_STREAM_INFRARED, width, height, frameRate, 1)) return false;

    m_depthProfiles.push_back(infraredProfile);

    //Enable Y16 format for Right
    stream_profile infraredProfile2;
    if (!GetProfile(infraredProfile2, rs2_stream::RS2_STREAM_INFRARED, width, height, frameRate, 2)) return false;

    m_depthProfiles.push_back(infraredProfile2);

    if (enableColor)
    {
        stream_profile profile;
        if (!GetProfile(profile, rs2_stream::RS2_STREAM_COLOR, colorWidth, colorHeight, frameRate, 0)) return false;

        m_colorProfile.push_back(profile);
    }

	m_lrImage[0] = std::unique_ptr<uint16_t[]>(new uint16_t[width*(height + 1)]);
	m_lrImage[1] = std::unique_ptr<uint16_t[]>(new uint16_t[width*(height + 1)]);

    return true;
}

void Rs400Device::StartCapture(std::function<void(const void *leftImage, const void *rightImage,
    const void *depthImage, const uint64_t timeStamp)> callback)
{
    m_callback = callback;

    if (!m_callback)
        throw std::runtime_error("SetCallback() must be called before StartCapture()!");

    if (m_depthProfiles.size() == 0)
        throw std::runtime_error("SetMediaMode() must be called before StartCapture()!");

    if (m_captureStarted) return;

    m_depthSensor.open(m_depthProfiles);
    try
    {
        m_depthSensor.start([&](rs2::frame f) {
            auto profile = f.get_profile();
            auto stream_type = profile.stream_type();
            auto video = profile.as<video_stream_profile>();

            if (stream_type != rs2_stream::RS2_STREAM_INFRARED)
                return;

            RS_400_STREAM_TYPE rs400StreamType;
            rs400StreamType = (profile.stream_index() == 1) ? RS400_STREAM_INFRARED : RS400_STREAM_INFRARED2;

            MUTEX_LOCK(&m_mutex);

            m_timestamp[(int)rs400StreamType] =
                (uint64_t)f.get_frame_metadata(rs2_frame_metadata_value::RS2_FRAME_METADATA_TIME_OF_ARRIVAL);

            m_ts = m_timestamp[(int)rs400StreamType];

            m_pData[(int)rs400StreamType] = (void *)f.get_data();

            int size = 2*video.width() * video.height();
            uint16_t *lr = m_lrImage[(int)rs400StreamType].get();
#ifdef _WIN32
            memcpy_s((void *)lr, size, m_pData[(int)rs400StreamType], size);
#else
            memcpy((void *)lr, m_pData[(int)rs400StreamType], size);
#endif

            if (m_timestamp[RS400_STREAM_INFRARED] != m_timestamp[RS400_STREAM_INFRARED2] || m_bColorEnabled)
            {
                MUTEX_UNLOCK(&m_mutex);
                return;
            }

            //process
            uint8_t *left = (uint8_t *)m_lrImage[0].get();
            uint8_t *right = (uint8_t *)m_lrImage[1].get();
            uint8_t *color = nullptr;

            m_callback(left, right, color, m_ts);

            MUTEX_UNLOCK(&m_mutex);
        });
    }
    catch (...)
    {
        m_depthSensor.close();
        throw;
    }

	if (m_colorProfile.size() > 0)
	{
		m_colorSensor.open(m_colorProfile);
		try
		{
			m_colorSensor.start([&](rs2::frame f) {
				auto profile = f.get_profile();
				auto stream_type = profile.stream_type();
				auto video = profile.as<video_stream_profile>();

				if (stream_type != rs2_stream::RS2_STREAM_COLOR) return;

				MUTEX_LOCK(&m_mutex);

				m_timestamp[(int)RS400_STREAM_COLOR] =
					(uint64_t)f.get_frame_metadata(rs2_frame_metadata_value::RS2_FRAME_METADATA_TIME_OF_ARRIVAL);

				m_pData[(int)RS400_STREAM_COLOR] = (void *)f.get_data();

				if (m_timestamp[(int)RS400_STREAM_COLOR] < m_ts)
				{
					MUTEX_UNLOCK(&m_mutex);
					return;
				}

				//process
				uint8_t *left = (uint8_t *)m_lrImage[0].get();
				uint8_t *right = (uint8_t *)m_lrImage[1].get();
				uint8_t* color = (uint8_t *)m_pData[(int)RS400_STREAM_COLOR];

				m_callback(left, right, color, m_ts);

				m_ts = m_timestamp[(int)RS400_STREAM_COLOR];

				MUTEX_UNLOCK(&m_mutex);
			});
		}
		catch (...)
		{
			m_colorSensor.close();
			throw;
		}
	}


    m_captureStarted = true;
}

void Rs400Device::StopCapture()
{
    if (!m_captureStarted) return;

	if (m_colorProfile.size() > 0)
	{
		m_colorSensor.stop();
		m_colorSensor.close();
	}

    m_depthSensor.stop();
    m_depthSensor.close();

    m_captureStarted = false;
}


void Rs400Device::EnableAutoExposure(float value)
{
    m_depthSensor.set_option(rs2_option::RS2_OPTION_ENABLE_AUTO_EXPOSURE, value);
}

void Rs400Device::EnableEmitter(float value)
{
	if (m_depthSensor.supports(rs2_option::RS2_OPTION_EMITTER_ENABLED))
	{
		m_depthSensor.set_option(rs2_option::RS2_OPTION_EMITTER_ENABLED, value);
	}
}

void Rs400Device::SetAeControl(unsigned int point)
{
	rs400::advanced_mode advanced = m_device.as<rs400::advanced_mode>();
	STAEControl aeControl = { point };
	advanced.set_ae_control(aeControl);
}

bool Rs400Device::GetProfile(stream_profile& profile, rs2_stream stream, int width, int height,
    int fps, int index)
{
	rs2::sensor sensor;
	rs2_format format;
	
	if (stream == rs2_stream::RS2_STREAM_INFRARED)
	{
		sensor = m_depthSensor;
		format = rs2_format::RS2_FORMAT_Y16;
	}
	else
	{
		sensor = m_colorSensor;
		format = rs2_format::RS2_FORMAT_YUYV;
	}

    vector<stream_profile> pfs = sensor.get_stream_profiles();

    for (int i = 0; i < (int)pfs.size(); i++)
    {
        auto video = pfs[i].as<video_stream_profile>();

        if ((pfs[i].format() == format)
            && (video.width() == width)
            && (video.height() == height)
            && (video.fps() == fps)
            && (video.stream_index() == index)
            )
        {
            profile = pfs[i];
            return true;
        }
    }

    return false;
}
