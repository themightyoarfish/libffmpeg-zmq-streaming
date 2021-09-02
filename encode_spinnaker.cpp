#include "avreceiver.hpp"
#include "avtransmitter.hpp"
#include "avutils.hpp"
#include <chrono>
#include <csignal>
#include <fcntl.h>
#include <iostream>
#include <linux/videodev2.h>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <sys/ioctl.h>
#include <thread>
#include <vector>

#include "SpinGenApi/SpinnakerGenApi.h"
#include "Spinnaker.h"

using std::string;
using namespace Spinnaker::GenApi;

static Spinnaker::SystemPtr spinnaker_system = nullptr;
static Spinnaker::CameraPtr camera = nullptr;
static Spinnaker::CameraList camList;
static Spinnaker::ImagePtr currentFrame = nullptr;

static volatile bool stop = false;

void shutdown_camera(int signal) {
  stop = true;
  std::cout << "Shitting down cameras." << std::endl;

  if (currentFrame) {
    try {
      std::cout << "Release current frame" << std::endl;
      currentFrame->Release();
    } catch (Spinnaker::Exception &e) {
      std::cout << "Caught error " << e.what() << std::endl;
    }
    currentFrame = nullptr;
  }
  std::cout << "End acquisition" << std::endl;
  camera->EndAcquisition();
  std::cout << "Deinit camera" << std::endl;
  camera->DeInit();
  // when spinnaker_system  is released in same scope, camera ptr must be
  // cleaned up before
  camera = nullptr;
  std::cout << "Clear camera list" << std::endl;
  camList.Clear();
  std::cout << "Release system" << std::endl;
  spinnaker_system->ReleaseInstance();
  spinnaker_system = nullptr;
}

int setCameraSetting(const string &node, const string &value) {
  INodeMap &nodeMap = camera->GetNodeMap();

  // Retrieve enumeration node from nodemap
  CEnumerationPtr ptr = nodeMap.GetNode(node.c_str());
  if (!IsAvailable(ptr)) {
    return -1;
  }
  if (!IsWritable(ptr)) {
    return -1;
  }
  // Retrieve entry node from enumeration node
  CEnumEntryPtr ptrValue = ptr->GetEntryByName(value.c_str());
  if (!IsAvailable(ptrValue)) {
    return -1;
  }
  if (!IsReadable(ptrValue)) {
    return -1;
  }
  // retrieve value from entry node
  const int64_t valueToSet = ptrValue->GetValue();

  // Set value from entry node as new value of enumeration node
  ptr->SetIntValue(valueToSet);

  return 0;
}

int setCameraSetting(const string &node, int val) {
  INodeMap &nodeMap = camera->GetNodeMap();

  CIntegerPtr ptr = nodeMap.GetNode(node.c_str());
  if (!IsAvailable(ptr)) {
    return -1;
  }
  if (!IsWritable(ptr)) {
    return -1;
  }
  ptr->SetValue(val);
  return 0;
}
int setCameraSetting(const string &node, float val) {
  INodeMap &nodeMap = camera->GetNodeMap();
  CFloatPtr ptr = nodeMap.GetNode(node.c_str());
  if (!IsAvailable(ptr)) {
    return -1;
  }
  if (!IsWritable(ptr)) {
    return -1;
  }
  ptr->SetValue(val);

  return 0;
}
int setCameraSetting(const string &node, bool val) {
  INodeMap &nodeMap = camera->GetNodeMap();

  CBooleanPtr ptr = nodeMap.GetNode(node.c_str());
  if (!IsAvailable(ptr)) {
    return -1;
  }
  if (!IsWritable(ptr)) {
    return -1;
  }
  ptr->SetValue(val);

  return 0;
}

int setPixFmt() { return setCameraSetting("PixelFormat", string("BayerBG8")); }

void resetUserSet() {
  std::this_thread::sleep_for(std::chrono::seconds(1));
  camera->UserSetSelector.SetValue(
      Spinnaker::UserSetSelectorEnums::UserSetSelector_Default);

  camera->UserSetLoad.Execute();
  std::this_thread::sleep_for(std::chrono::seconds(1));
}

int setExposureTime(float exposure_time) {
  return setCameraSetting("ExposureTime", exposure_time);
}

int setExposureAuto(const string &mode) {
  return setCameraSetting("ExposureAuto", mode);
}

int setGainAutoDisable() { return setCameraSetting("GainAuto", string("Off")); }

int setSharpeningDisable() {
  return setCameraSetting("SharpeningEnable", false);
}

int setWhiteBalanceAuto() {
  return setCameraSetting("BalanceWhiteAuto", string("Continuous"));
}

int main(int argc, char *argv[]) {
  // EndAcquisition() blocks indefinitely, so can't shut down (used to work)
  std::signal(SIGINT, shutdown_camera);

  constexpr int width = 1280;
  constexpr int height = 1024;
  constexpr int offsetX = 0;
  constexpr int offsetY = 0;
  std::string serial = "19450079";

  AVTransmitter transmitter("*", 15001, 15);

  if (argc > 1) {
    serial = argv[1];
  } else {
    std::cout << "Usage: " << argv[0] << " <serial>" << std::endl;
    return 1;
  }

  spinnaker_system = Spinnaker::System::GetInstance();

  // Retrieve list of cameras from the spinnaker_system
  camList = spinnaker_system->GetCameras();

  camera = camList.GetBySerial(serial);

  camList.Clear();

  // Initialize camera
  std::cout << "Init camera" << std::endl;
  camera->Init();

  /* resetUserSet(); */

  std::cout << "Setting params" << std::endl;
  // Set acquisition mode to continuous
  if (setCameraSetting("AcquisitionMode", string("Continuous")) == -1) {
    throw std::runtime_error("Could not set AcquisitionMode");
  }

  setCameraSetting("AcquisitionFrameRateEnabled", true);
  setCameraSetting("AcquisitionFrameRateEnable", true);
  setCameraSetting("AcquisitionFrameRateAuto", std::string("Off"));
  setCameraSetting("AcquisitionFrameRate", 60);

  // Important, otherwise we don't get frames at all
  if (setPixFmt() == -1) {
    throw std::runtime_error("Could not set pixel format");
  }

  // Disable auto exposure to be safe. otherwise we cannot manually set
  // exposure time.
  setCameraSetting("ExposureAuto", string("On"));

  std::cout << "Beginning acquisition" << std::endl;
  camera->BeginAcquisition();

  // wait a bit for camera to start streaming
  std::this_thread::sleep_for(std::chrono::seconds(1));

  std::cout << "Setting up stream " << std::endl;

  std::cout << "Beginning capture." << std::endl;

  while (!stop) {
    try {
      currentFrame = camera->GetNextImage();
      if (currentFrame->IsIncomplete()) {
        currentFrame->Release();
        currentFrame = nullptr;
      } else if (currentFrame->GetImageStatus() != Spinnaker::IMAGE_NO_ERROR) {
        currentFrame->Release();
        currentFrame = nullptr;
      }
    } catch (const Spinnaker::Exception &e) {
    }

    if (currentFrame) {
      Spinnaker::ImagePtr convertedImage;

      convertedImage = currentFrame->Convert(Spinnaker::PixelFormat_RGB8,
                                             Spinnaker::NEAREST_NEIGHBOR);
      currentFrame->Release();
      currentFrame = convertedImage;

      cv::Mat image(currentFrame->GetHeight() + currentFrame->GetYPadding(),
                    currentFrame->GetWidth() + currentFrame->GetXPadding(),
                    CV_8UC3, currentFrame->GetData(),
                    currentFrame->GetStride());

      auto tic = std::chrono::system_clock::now();
      transmitter.encode_frame(image);
      auto toc = std::chrono::system_clock::now();
      /* cout << "fps = " << 1 / ((ms / 1000.0) / (i + 1)) << endl; */
      /* currentFrame->Release(); */
      currentFrame = nullptr;
    }
  }

  return 0;
}