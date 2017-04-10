#include <layers/dm_layer_conv.hpp>
#include <dm.hpp>
#include <clblast_c.h>
#include <clblast.h>

using namespace deepmon;
namespace deepmon {
    void DM_Layer_Conv::CAFFE_LAYOUT_im2col_gpu(DM_Blob *input, DM_Blob *output) {
        int batches = input->get_shape_at(0);

        for(int b = 0 ; b < batches ; b++) {
            uint32_t input_offset = b * input->get_shape_at(1) * input->get_shape_at(2) * input->get_shape_at(3);
            uint32_t output_offset = b * output->get_shape_at(1) * output->get_shape_at(2) * output->get_shape_at(3);

            DeepMon::Get().GetGpuExecutionEngine().ExecuteIm2Col(
                    MEMORY_LAYOUT_CAFFE, this->precision, input, input_offset,
                    filter_h, filter_w, stride_h, stride_w,
                    pad_left, pad_top, pad_right, pad_bottom,
                    dilation_h, dilation_w, output_h, output_w,
                    output, output_offset);
        }
    }

    void DM_Layer_Conv::CAFFE_LAYOUT_conv_gpu(DM_Blob *input, DM_Blob *output) {

        std::vector<uint32_t> im2col_shapes{
                input->get_shape_at(CAFFE_BLOB_INOUT_BATCH_IDX),
                num_channels * filter_h * filter_w,
                output->get_shape_at(CAFFE_BLOB_INOUT_HEIGHT_IDX),
                output->get_shape_at(CAFFE_BLOB_INOUT_WIDTH_IDX)
        };

        DM_Blob *im2col_blob = new DM_Blob(im2col_shapes, ENVIRONMENT_GPU, this->precision,
                                           NULL);

        CAFFE_LAYOUT_im2col_gpu(input, im2col_blob);

        int input_offset = im2col_blob->get_shape_at(1) * im2col_blob->get_shape_at(2) *
                           im2col_blob->get_shape_at(3);
        int output_offset =
                output->get_shape_at(1) * output->get_shape_at(2) * output->get_shape_at(3);

        int m = filters->get_shape_at(CAFFE_BLOB_FILTER_NUM_FILTERS);
        int k = im2col_blob->get_shape_at(1);
        int n = output->get_shape_at(CAFFE_BLOB_INOUT_HEIGHT_IDX) *
                output->get_shape_at(CAFFE_BLOB_FILTER_WIDTH);

        float *biases_multiplier = NULL;
        if (biases != NULL) {
            biases_multiplier = new float[n];
            for (int i = 0; i < n; i++)
                biases_multiplier[i] = 1;
        }

        DM_Blob *biases_multiplier_blob = NULL;
        if (biases != NULL) {
            biases_multiplier_blob = new DM_Blob(vector<uint32_t>({(uint32_t)n}), ENVIRONMENT_GPU,
                                                 this->precision, biases_multiplier);
        }

        cl_command_queue queue = DeepMon::Get().GetGpuExecutionEngine().GetCurrentQueue();
        for (int b = 0; b < input->get_shapes()[0]; b++) {
            cl_event event;
            CLBlastStatusCode status;
            if (precision == PRECISION_32) {
                status = CLBlastSgemm(CLBlastLayoutRowMajor,
                                                        CLBlastTransposeNo, CLBlastTransposeNo,
                                                        m, n, k,
                                                        1.0f,
                                                        filters->get_gpu_data(), 0, k,
                                                        im2col_blob->get_gpu_data(),
                                                        b * input_offset, n,
                                                        0,
                                                        output->get_gpu_data(),
                                                        b * output_offset, n,
                                                        &queue, &event);
            } else {
                status = CLBlastHgemm(CLBlastLayoutRowMajor,
                                                        CLBlastTransposeNo, CLBlastTransposeNo,
                                                        m, n, k,
                                                        1.0f,
                                                        filters->get_gpu_data(), 0, k,
                                                        im2col_blob->get_gpu_data(),
                                                        b * input_offset, n,
                                                        0,
                                                        output->get_gpu_data(),
                                                        b * output_offset, n,
                                                        &queue, &event);
            }

            if (status == CLBlastSuccess) {
                clWaitForEvents(1, &event);
                clReleaseEvent(event);
            } else {
                LOGE("[%s]: Gemm_1 failed with status %d", this->name.c_str(), status);
                output->set_corrupted(true);
                break;
            }

            if (biases != NULL) {
                if(precision == PRECISION_32) {
                    status = CLBlastSgemm(
                            CLBlastLayoutRowMajor,
                            CLBlastTransposeNo, CLBlastTransposeNo,
                            biases->get_shape_at(0), n, 1,
                            1.0f,
                            biases->get_gpu_data(), 0, 1,
                            biases_multiplier_blob->get_gpu_data(), 0, n,
                            1.0f, output->get_gpu_data(),
                            b * output->get_total_size() / output->get_shape_at(0), n,
                            &queue, &event);
                } else {
                    status = CLBlastHgemm(
                            CLBlastLayoutRowMajor,
                            CLBlastTransposeNo, CLBlastTransposeNo,
                            biases->get_shape_at(0), n, 1,
                            1.0f,
                            biases->get_gpu_data(), 0, 1,
                            biases_multiplier_blob->get_gpu_data(), 0, n,
                            1.0f, output->get_gpu_data(),
                            b * output->get_total_size() / output->get_shape_at(0), n,
                            &queue, &event);
                }

                if (status == CLBlastSuccess) {
                    clWaitForEvents(1, &event);
                    clReleaseEvent(event);
                } else {
                    LOGE("[%s]: Gemm_2 failed with status %d", this->name.c_str(), status);
                    output->set_corrupted(true);
                    break;
                }
            }
        }
        
        delete im2col_blob;

        if(biases != NULL) {
            delete biases_multiplier;
            delete biases_multiplier_blob;
        }
    }

    DM_Blob* DM_Layer_Conv::do_conv_gpu(DM_Blob *input) {
        //need to add batch_size
        DM_Blob *output = new DM_Blob(vector<uint32_t> {
                input->get_shape_at(0), output_shapes[0], output_shapes[1], output_shapes[2]
        }, ENVIRONMENT_GPU, this->precision, NULL);

        if (mem_layout == MEMORY_LAYOUT_CAFFE) {
            CAFFE_LAYOUT_conv_gpu(input, output);
        } else if(mem_layout == MEMORY_LAYOUT_DM) {
            /*
             * Fixme: Please implement soon
             */
        }

        return output;
    }
}