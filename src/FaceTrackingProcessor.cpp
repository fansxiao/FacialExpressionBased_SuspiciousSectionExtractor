#include "stdafx.h"
#include "FaceTrackingProcessor.h"
#include <assert.h>
#include <string>
#include "pxcfaceconfiguration.h"
#include "pxcfacemodule.h"
#include "pxcsensemanager.h"
#include "FaceTrackingUtilities.h"
#include "FaceTrackingAlertHandler.h"
#include "FaceTrackingRendererManager.h"
#include "resource.h"
//#include <vld.h>
extern PXCSession* session;
extern FaceTrackingRendererManager* renderer;
// sdfwefwe

extern volatile bool isStopped;
extern volatile bool isActiveApp;
extern pxcCHAR fileName[1024];
extern pxcCHAR TextfileName[1024];
extern HANDLE ghMutex;

FaceTrackingProcessor::FaceTrackingProcessor(HWND window) : m_window(window), m_registerFlag(false), m_unregisterFlag(false) { Framenumber = 0; }

void FaceTrackingProcessor::PerformRegistration()
{
	m_registerFlag = false;
	if(m_output->QueryFaceByIndex(0))
		m_output->QueryFaceByIndex(0)->QueryRecognition()->RegisterUser();
}

void FaceTrackingProcessor::PerformUnregistration()
{
	m_unregisterFlag = false;
	if(m_output->QueryFaceByIndex(0))
		m_output->QueryFaceByIndex(0)->QueryRecognition()->UnregisterUser();
}

void FaceTrackingProcessor::CheckForDepthStream(PXCSenseManager* pp, HWND hwndDlg)
{
	PXCFaceModule* faceModule = pp->QueryFace();
	if (faceModule == NULL) 
	{
		assert(faceModule);
		return;
	}
	PXCFaceConfiguration* config = faceModule->CreateActiveConfiguration();
	if (config == NULL)
	{
		assert(config);
		return;
	}

	PXCFaceConfiguration::TrackingModeType trackingMode = config->GetTrackingMode();
	config->Release();
	if (trackingMode == PXCFaceConfiguration::FACE_MODE_COLOR_PLUS_DEPTH)
	{
		PXCCapture::Device::StreamProfileSet profiles={};
		pp->QueryCaptureManager()->QueryDevice()->QueryStreamProfileSet(&profiles);
		if (!profiles.depth.imageInfo.format)
		{            
			std::wstring msg = L"Depth stream is not supported for device: ";
			msg.append(FaceTrackingUtilities::GetCheckedDevice(hwndDlg));           
			msg.append(L". \nUsing 2D tracking");
			MessageBox(hwndDlg, msg.c_str(), L"Face Tracking", MB_OK);            
		}
	}
}

void FaceTrackingProcessor::Process(HWND dialogWindow)
{
	int min, sec;
	CString str;

	senseManager = session->CreateSenseManager();
	if (senseManager == NULL) 
	{
		FaceTrackingUtilities::SetStatus(dialogWindow, L"Failed to create an SDK SenseManager", statusPart);
		return;
	}

	/* Set Mode & Source */
	PXCCaptureManager* captureManager = senseManager->QueryCaptureManager();
	if (!FaceTrackingUtilities::GetPlaybackState(dialogWindow))
	{
		captureManager->FilterByDeviceInfo(FaceTrackingUtilities::GetCheckedDeviceInfo(dialogWindow));
	}

	pxcStatus status = PXC_STATUS_NO_ERROR;
	if (FaceTrackingUtilities::GetRecordState(dialogWindow)) 
	{
		status = captureManager->SetFileName(fileName, true);
	} 
	else if (FaceTrackingUtilities::GetPlaybackState(dialogWindow)) 
	{
		status = captureManager->SetFileName(fileName, false);
		senseManager->QueryCaptureManager()->SetRealtime(false);
		Framenumber=captureManager->QueryNumberOfFrames();
		HWND slider = GetDlgItem(dialogWindow, IDC_SLIDER);
		SendMessage(slider, TBM_SETRANGE, FALSE, MAKELPARAM(0, Framenumber));
		HWND total = GetDlgItem(dialogWindow, IDC_TTIME);
		min =(int)Framenumber / 1800;
		sec =(int)(Framenumber / 30) - (60 * min);
		str.Format(_T(" %dm %ds"), min, sec);
		SetWindowTextW(total, str);
	} 
	if (status < PXC_STATUS_NO_ERROR) 
	{
		FaceTrackingUtilities::SetStatus(dialogWindow, L"Failed to Set Record/Playback File", statusPart);
		return;
	}

	/* Set Module */
	senseManager->EnableFace();

	/* Initialize */
	FaceTrackingUtilities::SetStatus(dialogWindow, L"Init Started", statusPart);

	PXCFaceModule* faceModule = senseManager->QueryFace();
	if (faceModule == NULL)
	{
		assert(faceModule);
		return;
	}
	PXCFaceConfiguration* config = faceModule->CreateActiveConfiguration();
	if (config == NULL)
	{
		assert(config);
		return;
	}
	config->SetTrackingMode(FaceTrackingUtilities::GetCheckedProfile(dialogWindow));

	//시선추적 추가 데이터
	PXCFaceConfiguration::GazeConfiguration *gazec = config->QueryGaze();
	gazec->isEnabled = true;
	//

	config->ApplyChanges();

	if (!FaceTrackingUtilities::GetPlaybackState(dialogWindow))
	{
		PXCCapture::Device::StreamProfileSet set;
		memset(&set, 0, sizeof(set));
		set.color.imageInfo.height = 720;
		set.color.imageInfo.width = 1280;	
		captureManager->FilterByStreamProfiles(&set);
	}
		
	if (senseManager->Init() < PXC_STATUS_NO_ERROR)
	{
		captureManager->FilterByStreamProfiles(NULL);
		if (senseManager->Init() < PXC_STATUS_NO_ERROR)
		{
			FaceTrackingUtilities::SetStatus(dialogWindow, L"Init Failed", statusPart);
			config->Release();
			senseManager->Close();
			senseManager->Release();
			return;
		}
	}

	PXCCapture::DeviceInfo info;
	senseManager->QueryCaptureManager()->QueryDevice()->QueryDeviceInfo(&info);

    CheckForDepthStream(senseManager, dialogWindow);
    FaceTrackingAlertHandler alertHandler(dialogWindow);
    if (FaceTrackingUtilities::GetCheckedModule(dialogWindow))
    {
		//detection 미사용
        config->detection.isEnabled = true;

        config->landmarks.isEnabled = true;
        config->pose.isEnabled = true;
		config->QueryPulse()->Enable();
        config->QueryExpressions()->Enable();
        config->QueryExpressions()->EnableAllExpressions();

		//얼굴인식 미사용
		config->QueryRecognition()->Disable();

        config->EnableAllAlerts();
        config->SubscribeAlert(&alertHandler);

        config->ApplyChanges();
    }
    FaceTrackingUtilities::SetStatus(dialogWindow, L"Streaming", statusPart);
    m_output = faceModule->CreateOutput();

    bool isNotFirstFrame = false;
    bool isFinishedPlaying = false;

    ResetEvent(renderer->GetRenderingFinishedSignal());

	renderer->SetSenseManager(senseManager);
    renderer->SetNumberOfLandmarks(config->landmarks.numLandmarks);
    renderer->SetCallback(renderer->SignalProcessor);

    if (!isStopped)
    {
        while (true)
        {
            if (senseManager->AcquireFrame(true) < PXC_STATUS_NO_ERROR)
            {
                isFinishedPlaying = true;
            }

            if (isNotFirstFrame)
            {
                WaitForSingleObject(renderer->GetRenderingFinishedSignal(), INFINITE);
            }

            if (isFinishedPlaying || isStopped)
            {
                if (isStopped)
                    senseManager->ReleaseFrame();

                if (isFinishedPlaying)
                    PostMessage(dialogWindow, WM_COMMAND, ID_STOP, 0);

                break;
            }

            m_output->Update();
            PXCCapture::Sample* sample = senseManager->QueryFaceSample();

            isNotFirstFrame = true;

            if (sample != NULL)
            {
				DWORD dwWaitResult;
				dwWaitResult = WaitForSingleObject(ghMutex,	INFINITE);
				if(dwWaitResult == WAIT_OBJECT_0)
				{
					renderer->DrawBitmap(
						sample, config->GetTrackingMode() == PXCFaceConfiguration::FACE_MODE_IR);
					renderer->DrawBitmap2(
						sample, config->GetTrackingMode() == PXCFaceConfiguration::FACE_MODE_IR);
					renderer->SetOutput(m_output);
					renderer->SignalRenderer();
					if(!ReleaseMutex(ghMutex))
					{
						throw std::exception("Failed to release mutex");
						return;
					}
				}
            }

            if (config->QueryRecognition()->properties.isEnabled)
            {
                if (m_registerFlag)
                    PerformRegistration();

                if (m_unregisterFlag)
                    PerformUnregistration();
            }

			// Resume next frame processing
            senseManager->ReleaseFrame();
        }

        m_output->Release();
        FaceTrackingUtilities::SetStatus(dialogWindow, L"Stopped", statusPart);
    }

	config->Release();
	senseManager->Close(); 
	senseManager->Release();
}

void FaceTrackingProcessor::RegisterUser()
{
	m_registerFlag = true;
}

void FaceTrackingProcessor::UnregisterUser()
{
	m_unregisterFlag = true;
}