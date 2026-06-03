#ifndef NOTCH_MOCK_LOCAL_CUDA_SHARE_READER_H
#define NOTCH_MOCK_LOCAL_CUDA_SHARE_READER_H

#include "matrix.h"

namespace notch_mock
{

class LocalCudaShareReader : public ICudaShareReader
{
public:
    bool ReadShare(
        const notch_comfy::CudaShareStatus& share,
        std::vector<uint8_t>& bytes,
        std::string& error) override;
};

} // namespace notch_mock

#endif
