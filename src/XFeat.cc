#include "XFeat.h"
#include "OnnxHelper.h"


XFeat::XFeat(const std::string &modelFile) {
    // Convert the modelFile path to onnx compatible path
    std::vector<ORTCHAR_T> modelFileOrt;
    OnnxHelper::Str2Ort(modelFile, modelFileOrt);

    // create onnx runtime session
    ortEnv_ = std::unique_ptr<Ort::Env>(new Ort::Env(ORT_LOGGING_LEVEL_WARNING, "XFeat"));

    Ort::SessionOptions sessionOptions;
    sessionOptions.SetIntraOpNumThreads(1);
    sessionOptions.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_BASIC);

    ortSession_ = std::unique_ptr<Ort::Session>(new Ort::Session(*ortEnv_, modelFileOrt.data(), sessionOptions));

    // Get model info
    OnnxHelper::GetModelInfo(*ortSession_, inputInfos_, outputInfos_);

    // set input and output names
    for (const auto &inputInfo : inputInfos_) {
        inputNames_.push_back(inputInfo.name.c_str());
    }
    for (const auto &outputInfo : outputInfos_) {
        outputNames_.push_back(outputInfo.name.c_str());
    }

    H_ = (int)inputInfos_[0].shape[2];
    W_ = (int)inputInfos_[0].shape[3];
    Hd8_ = H_ / 8;
    Wd8_ = W_ / 8;

    // make sure H and W can be divided by 32
    if (H_ % 32 != 0 || W_ % 32 != 0) {
        std::cerr << "Input image size must be divisible by 32!" << std::endl;
        exit(-1);
    }

    // print model info
    OnnxHelper::PrintModelInfo(inputInfos_, outputInfos_);
}



void XFeat::DetectAndCompute(const cv::Mat &img, std::vector<cv::KeyPoint> &keys, cv::Mat &descs, int maxCorners) {
    // check image size
    if (img.channels() != inputInfos_[0].shape[1]) {
        std::cerr << "Image channel mismatch!" << img.channels() << std::endl;
        return;
    }

    if (img.rows != H_ || img.cols != W_) {
        std::cerr << "Image size mismatch!" << img.rows << ", " << img.cols << std::endl;
        return;
    }

    // convert image to tensor
    cv::Mat fimg;
    img.convertTo(fimg, CV_32F, 1.0/255.0);
    std::vector<Ort::Value> inputTensors;
    inputTensors.emplace_back(OnnxHelper::CreateTensor<float>(inputInfos_[0].shape, fimg.ptr<float>(), img.rows * img.cols, true));

    // run inference
    // outputTensors:
    // 0: [1, 64, H/8, W/8] descriptors
    // 1: [1, 65, H/8, W/8] keypoint scores
    // 2: [1, 1, H/8, W/8] reliability map
    auto outputTensors = ortSession_->Run(Ort::RunOptions{nullptr}, inputNames_.data(),
                                          inputTensors.data(), 1, outputNames_.data(), outputNames_.size());

    // get the keypoint scores, it's a [1, 65, H/8, W/8] tensor,
    // we shall apply softmax along the 65 channels to get the scores
    auto* kptScorePtr = outputTensors[1].GetTensorMutableData<float>();
    const int shw = Hd8_ * Wd8_;
    for (int i = 0; i < shw; ++i) {
        float sum = 0;
        for (int j = 0; j < 65; ++j) {
            sum += std::exp(kptScorePtr[j * shw + i]);
        }
        for (int j = 0; j < 65; ++j) {
            kptScorePtr[j * shw + i] = std::exp(kptScorePtr[j * shw + i]) / sum;
        }
    }

    // the keypoint score tensor [1, 65, H/8, W/8], we drop the last channel(dust bin), and convert to [H, W] image
    cv::Mat scoreImg(H_, W_, CV_32F);
    for (int i = 0; i < 64; ++i) {
        const int ir = i / 8;
        const int ic = i % 8;
        const int iShw = i * shw;
        for (int k = 0; k < Hd8_; ++k) {
            const int row = k * 8 + ir;
            const int kWd8 = k * Wd8_;
            for (int j = 0; j < Wd8_; ++j) {
                int col = j * 8 + ic;
                scoreImg.at<float>(row, col) = kptScorePtr[iShw + kWd8 + j];
            }
        }
    }

    // apply nms, only keep the points with score > 0.05 and is the local maxima in a 5x5 window
    Nms(scoreImg, 0.05f, nmsKernelSize_, scoredPoints_);

    // get the reliability map [1, 1, H/8, W/8]
    auto* heatMapPtr = outputTensors[2].GetTensorMutableData<float>();
    cv::Mat heatMapSmall(Hd8_, Wd8_, CV_32F, heatMapPtr);
    // resize it to [H, W]
    cv::Mat heaMapFull;
    cv::resize(heatMapSmall, heaMapFull, cv::Size(W_, H_));

    // multiply the point score with reliability
    for (auto &pt : scoredPoints_) {
        pt.score *= heaMapFull.at<float>(pt.y, pt.x);
    }

    // sort the points by score
    std::sort(scoredPoints_.begin(), scoredPoints_.end(), [](const ScoredPoint &a, const ScoredPoint &b) {
        return a.score > b.score;
    });

    // only keep the top maxCorners points
    if (scoredPoints_.size() > maxCorners) {
        scoredPoints_.resize(maxCorners);
    }

    // convert the scored points to cv::KeyPoint
    keys.clear();
    for (const auto &pt : scoredPoints_) {
        keys.emplace_back(pt.x, pt.y, 0);
    }

    // compute descriptors for selected points
    auto *descTensorPtr = outputTensors[0].GetTensorMutableData<float>();

    descs = cv::Mat::zeros((int)keys.size(), 64, CV_32F);
    for (int n = 0; n < (int)(keys.size()); ++n) {
        const auto &pt = keys[n];
        float x = pt.pt.x / 8.f;
        float y = pt.pt.y / 8.f;

        // interpolate
        int x0 = (int)x;
        int y0 = (int)y;
        int x1 = x0 + 1;
        int y1 = y0 + 1;
        float dx = x - static_cast<float>(x0);
        float dy = y - static_cast<float>(y0);
        float w00 = (1 - dx) * (1 - dy);
        float w01 = dx * (1 - dy);
        float w10 = (1 - dx) * dy;
        float w11 = dx * dy;

        auto* desc_n_ptr = descs.ptr<float>(n);
        for (int i = 0; i < 64; ++i) {
            int iShw = i * shw;
            float v00 = descTensorPtr[iShw + y0 * Wd8_ + x0];
            float v01 = descTensorPtr[iShw + y0 * Wd8_ + x1];
            float v10 = descTensorPtr[iShw + y1 * Wd8_ + x0];
            float v11 = descTensorPtr[iShw + y1 * Wd8_ + x1];
            desc_n_ptr[i] = w00 * v00 + w01 * v01 + w10 * v10 + w11 * v11;
        }
    }

}


void XFeat::Nms(const cv::Mat &scores, float scoreThresh, int kernelSize, std::vector<ScoredPoint> &points) {
    points.clear();

    int rows = scores.rows;
    int cols = scores.cols;
    int halfKernelSize = kernelSize / 2;
    cv::Mat mask = cv::Mat::ones(rows, cols, CV_8U);
    const auto * scorePtr = scores.ptr<float>();
    auto* maskPtr = mask.ptr<uchar>();

    std::vector<int> ptrOffsets;
    ptrOffsets.reserve(kernelSize * kernelSize);
    for (int i = -halfKernelSize; i <= halfKernelSize; i++) {
        for (int j = -halfKernelSize; j <= halfKernelSize; j++) {
            if (i == 0 && j == 0) {
                continue;
            }
            ptrOffsets.push_back(i * cols + j);
        }
    }

    for (int i = halfKernelSize; i < rows - halfKernelSize; i++) {
        for (int j = halfKernelSize; j < cols - halfKernelSize; j++) {
            int addr = i * cols + j;
            if (maskPtr[addr] == 0) {
                continue;
            }

            const float score = scorePtr[addr];
            if (score <= scoreThresh) {
                maskPtr[addr] = 0;
                continue;
            }

            // nms
            bool isMax = true;
            for (const auto &offset : ptrOffsets) {
                if (score < scorePtr[addr + offset]) {
                    maskPtr[addr] = 0;
                    isMax = false;
                    break;
                }
            }

            //
            if (isMax) {
                points.push_back({j, i, score});
                // mask out the neighbors
                for (const auto &offset : ptrOffsets) {
                    maskPtr[addr + offset] = 0;
                }
            }
        }
    }


}