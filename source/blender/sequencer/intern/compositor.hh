/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#pragma once

#include "BLI_vector.hh"

#include "COM_context.hh"
#include "COM_node_group_operation.hh"
#include "SEQ_render.hh"

struct PointerRNA;
struct bNodeTreeInterfaceSocket;
enum eNodeSocketDatatype : short;

namespace blender::seq {

struct SeqRenderState;

class CompositorContext : public compositor::Context {
 protected:
  const RenderData &render_data_;
  SeqRenderState &render_state_;
  const Strip *strip_ = nullptr;
  float timeline_frame_ = 0.0f;
  float2 result_translation_ = float2(0, 0);
  Vector<ImBuf *> strip_input_buffers_;

  /* True if GPU compute is supported and can be used, if false, we fallback to CPU. */
  bool gpu_supported_ = true;

 public:
  CompositorContext(compositor::StaticCacheManager &cache_manager,
                    const RenderData &render_data,
                    SeqRenderState &render_state,
                    const Strip &strip,
                    const float timeline_frame)
      : compositor::Context(cache_manager),
        render_data_(render_data),
        render_state_(render_state),
        strip_(&strip),
        timeline_frame_(timeline_frame)
  {
  }
  ~CompositorContext();
  const Main &get_main() const override
  {
    return *render_data_.bmain;
  }
  const Scene &get_scene() const override
  {
    return *render_data_.scene;
  }
  const Strip *get_strip() const override
  {
    return strip_;
  }

  void set_gpu_supported(const bool supported)
  {
    gpu_supported_ = supported;
  }

  bool use_gpu() const override
  {
    return gpu_supported_ &&
           this->render_data_.scene->r.compositor_device == SCE_COMPOSITOR_DEVICE_GPU;
  }

  compositor::ResultPrecision get_precision() const override;

  float2 get_result_translation() const
  {
    return result_translation_;
  }

 protected:
  enum class InputStripStatus {
    NotStrip,
    Loaded,
    Unavailable,
  };
  enum class InputImageStatus {
    NotImage,
    Loaded,
    Unavailable,
  };

  compositor::NodeGroupOutputTypes needed_outputs() const
  {
    if (!render_data_.render) {
      return compositor::NodeGroupOutputTypes::ViewerNode;
    }
    return compositor::NodeGroupOutputTypes();
  }

  void create_result_from_input(compositor::Result &result, ImBuf &input);
  /**
   * If the given color input is set to another sequencer strip, render that strip at the current
   * timeline frame and share its image into the result.
   */
  InputStripStatus fill_color_input_from_rna_strip(PointerRNA &inputs_ptr,
                                                   const bNodeTreeInterfaceSocket &socket,
                                                   compositor::Result &result);
  /** Load an Image data-block selected as the source of the given color input. */
  InputImageStatus fill_color_input_from_rna_image(PointerRNA &inputs_ptr,
                                                   const bNodeTreeInterfaceSocket &socket,
                                                   compositor::Result &result);
  /**
   * Allocate result as a single value or image, and fill it from the selected input source.
   */
  void set_input_result_from_rna(PointerRNA &inputs_ptr,
                                 const bNodeTreeInterfaceSocket &socket,
                                 eNodeSocketDatatype socket_type,
                                 compositor::Result &result);
  void write_viewer_impl(const compositor::Result &result, ImBuf &image);
  void write_output(const compositor::Result &result, ImBuf &image);
  void write_outputs(const bNodeTree &node_group,
                     compositor::NodeGroupOperation &node_group_operation,
                     ImBuf &output_image);
  void set_output_refcount(const bNodeTree &node_group,
                           compositor::NodeGroupOperation &node_group_operation);
};

}  // namespace blender::seq
