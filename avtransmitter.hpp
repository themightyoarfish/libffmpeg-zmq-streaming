#ifndef AVTRANSMITTER_HPP_A9X5A3XE
#define AVTRANSMITTER_HPP_A9X5A3XE

#include "avutils.hpp"
#include <chrono>
#include <iostream>
#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

#include <zmq.hpp>

class AVTransmitter {

  zmq::socket_t socket;
  zmq::context_t ctx;

  int num_pkts;
  size_t bytes_sent = 0;
  std::vector<std::uint8_t> imgbuf;
  AVFormatContext *ofmt_ctx = nullptr;
  AVCodec *out_codec = nullptr;
  AVStream *out_stream = nullptr;
  AVCodecContext *out_codec_ctx = nullptr;
  SwsContext *swsctx = nullptr;
  cv::Mat canvas_;
  unsigned int height_;
  unsigned int width_;
  unsigned int fps_;
  AVFrame *frame_ = nullptr;

public:
  AVTransmitter(const std::string &host, const unsigned int port,
                unsigned int fps)
      : ctx(1), fps_(fps) {
    socket = zmq::socket_t(ctx, zmq::socket_type::pub);
    socket.set(zmq::sockopt::sndhwm, 1);
    const auto bind_str =
        std::string("tcp://") + host + ":" + std::to_string(port);
    socket.bind(bind_str);
    std::cout << "Bound socket to " << bind_str << std::endl;
    num_pkts = 0;

    AVOutputFormat *format = av_guess_format("rtp", nullptr, nullptr);
    int success = avutils::initialize_avformat_context(
        this->ofmt_ctx, format, "rtp://192.168.19.201:49990");

    if (success != 0) {
      throw std::runtime_error("Could not allocate output format context! " +
                               avutils::av_strerror(success));
    }

    this->out_codec = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!this->out_codec) {
      throw std::runtime_error("Could not find encoder");
    }
    this->out_stream = avformat_new_stream(this->ofmt_ctx, this->out_codec);
    if (!this->out_stream) {
      throw std::runtime_error("Could not find stream");
    }
    this->out_codec_ctx = avcodec_alloc_context3(this->out_codec);
    if (!this->out_codec_ctx) {
      throw std::runtime_error("Could not allocate output codec context");
    }
  }

  void encode_frame(const cv::Mat &image) {
    static bool first_time = true;
    if (first_time) {
      first_time = false;
      height_ = image.rows;
      width_ = image.cols;
      avutils::set_codec_params(this->ofmt_ctx, this->out_codec_ctx, width_,
                                height_, fps_, 2e6, 6);
      int success = avutils::initialize_codec_stream(this->out_stream,
                                                     out_codec_ctx, out_codec);
      this->out_stream->time_base.num = 1;
      this->out_stream->time_base.den = fps_;
      avio_open(&(this->ofmt_ctx->pb), this->ofmt_ctx->filename,
                AVIO_FLAG_WRITE);

      /* Write a file for VLC */
      char buf[200000];
      AVFormatContext *ac[] = {this->ofmt_ctx};
      av_sdp_create(ac, 1, buf, 20000);
      printf("sdp:\n%s\n", buf);
      FILE *fsdp = fopen("test.sdp", "w");
      fprintf(fsdp, "%s", buf);
      fclose(fsdp);

      if (success != 0) {
        throw std::invalid_argument("Could not initialize codec stream " +
                                    avutils::av_strerror(success));
      }
      if (!swsctx) {
        swsctx = avutils::initialize_sample_scaler(this->out_codec_ctx, width_,
                                                   height_);
      }
      if (!swsctx) {
        throw std::runtime_error("Could not initialize sample scaler!");
      }
    }
    if (!frame_) {
      frame_ =
          avutils::allocate_frame_buffer(this->out_codec_ctx, width_, height_);
      int success = avformat_write_header(this->ofmt_ctx, nullptr);
      if (success != 0) {
        std::runtime_error("Could not write header! " +
                           avutils::av_strerror(success));
      }
    }
    if (imgbuf.empty()) {
      imgbuf.resize(height_ * width_ * 3 + 16);
      this->canvas_ =
          cv::Mat(height_, width_, CV_8UC3, imgbuf.data(), width_ * 3);
    } else {
      image.copyTo(this->canvas_);
    }
    const int stride[] = {static_cast<int>(image.step[0])};

    /* cv::imshow("encoded", image); */
    /* cv::waitKey(20); */
    sws_scale(this->swsctx, &canvas_.data, stride, 0, canvas_.rows,
              frame_->data, frame_->linesize);
    frame_->pts +=
        av_rescale_q(1, out_codec_ctx->time_base, this->out_stream->time_base);

    int success =
        avutils::write_frame(this->out_codec_ctx, this->ofmt_ctx, this->frame_);
    if (success != 0) {
      throw std::runtime_error("Could not write frame " +
                               avutils::av_strerror(success));
    } else {
      using namespace std::chrono;
      std::cout << "Encoded at " << std::setprecision(5) << std::fixed
                << duration_cast<milliseconds>(
                       system_clock::now().time_since_epoch())
                           .count() /
                       1000.0
                << std::endl;
      this->frame_ended();
    }
  }

  ~AVTransmitter() {
    av_write_trailer(this->ofmt_ctx);
    socket.close();
    av_frame_free(&frame_);
    avcodec_close(this->out_codec_ctx);
    avio_context_free(&(this->ofmt_ctx->pb));
    avformat_free_context(this->ofmt_ctx);
    std::cout << "Sent " << bytes_sent / KB << " KB" << std::endl;
  }

  void frame_ended() {

    socket.send(zmq::message_t());
    /* std::cout << "Sent " << ++num_pkts << std::endl; */
    num_pkts = 0;
  }
};

#endif /* end of include guard: AVTRANSMITTER_HPP_A9X5A3XE */
