/* SPDX-FileCopyrightText: 2023 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup nodes
 */

#pragma once

#include <cfloat>

#include "NOD_bundle_type.hh"
#include "NOD_menu_value.hh"
#include "NOD_node_declaration.hh"

#include "RNA_types.hh"

#include "BKE_node_enum.hh"

#include "BLI_color_types.hh"
#include "BLI_implicit_sharing_ptr.hh"
#include "BLI_math_euler_types.hh"
#include "BLI_math_vector_types.hh"

namespace blender::nodes {
class ClosureSignature;
}

namespace blender::nodes::decl {

class FloatBuilder;

class Float : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_FLOAT;

  float default_value = 0.0f;
  float soft_min_value = -FLT_MAX;
  float soft_max_value = FLT_MAX;
  PropertySubType subtype = PROP_NONE;

  friend FloatBuilder;

  using Builder = FloatBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class FloatBuilder : public SocketDeclarationBuilder<Float> {
 public:
  FloatBuilder &min(float value);
  FloatBuilder &max(float value);
  FloatBuilder &default_value(float value);
  FloatBuilder &subtype(PropertySubType subtype);
  FloatBuilder &try_copy_ui_data(const SocketDeclaration &other_decl) override;
};

class IntBuilder;

class Int : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_INT;

  int default_value = 0;
  int soft_min_value = INT32_MIN;
  int soft_max_value = INT32_MAX;
  PropertySubType subtype = PROP_NONE;

  friend IntBuilder;

  using Builder = IntBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class IntBuilder : public SocketDeclarationBuilder<Int> {
 public:
  IntBuilder &min(int value);
  IntBuilder &max(int value);
  IntBuilder &default_value(int value);
  IntBuilder &subtype(PropertySubType subtype);
  IntBuilder &try_copy_ui_data(const SocketDeclaration &other_decl) override;
};

class VectorBuilder;

class Vector : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_VECTOR;

  float4 default_value = {0, 0, 0, 0};
  float soft_min_value = -FLT_MAX;
  float soft_max_value = FLT_MAX;
  int dimensions = 3;
  PropertySubType subtype = PROP_NONE;

  friend VectorBuilder;

  using Builder = VectorBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class VectorBuilder : public SocketDeclarationBuilder<Vector> {
 public:
  VectorBuilder &default_value(const float2 value);
  VectorBuilder &default_value(const float3 value);
  VectorBuilder &default_value(const float4 value);
  VectorBuilder &subtype(PropertySubType subtype);
  VectorBuilder &dimensions(int dimensions);
  VectorBuilder &min(float min);
  VectorBuilder &max(float max);
  VectorBuilder &compact();
  VectorBuilder &try_copy_ui_data(const SocketDeclaration &other_decl) override;
};

class Vector2Builder;

class Vector2 : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_VECTOR2;

  float2 default_value = {0.0f, 0.0f};
  float soft_min_value = -FLT_MAX;
  float soft_max_value = FLT_MAX;
  PropertySubType subtype = PROP_NONE;

  friend Vector2Builder;

  using Builder = Vector2Builder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class Vector2Builder : public SocketDeclarationBuilder<Vector2> {
 public:
  Vector2Builder &default_value(const float2 value);
  Vector2Builder &subtype(PropertySubType subtype);
  Vector2Builder &min(float min);
  Vector2Builder &max(float max);
  Vector2Builder &compact();
  Vector2Builder &try_copy_ui_data(const SocketDeclaration &other_decl) override;
};

class Vector4Builder;

class Vector4 : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_VECTOR4;

  float4 default_value = {0.0f, 0.0f, 0.0f, 0.0f};
  float soft_min_value = -FLT_MAX;
  float soft_max_value = FLT_MAX;
  PropertySubType subtype = PROP_NONE;

  friend Vector4Builder;

  using Builder = Vector4Builder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class Vector4Builder : public SocketDeclarationBuilder<Vector4> {
 public:
  Vector4Builder &default_value(const float4 value);
  Vector4Builder &subtype(PropertySubType subtype);
  Vector4Builder &min(float min);
  Vector4Builder &max(float max);
  Vector4Builder &compact();
  Vector4Builder &try_copy_ui_data(const SocketDeclaration &other_decl) override;
};

class IntVectorBuilder;

class IntVector : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_INT_VECTOR;

  int3 default_value = {0, 0, 0};
  int soft_min_value = INT_MIN;
  int soft_max_value = INT_MAX;
  int dimensions = 3;
  PropertySubType subtype = PROP_NONE;

  friend IntVectorBuilder;

  using Builder = IntVectorBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class IntVectorBuilder : public SocketDeclarationBuilder<IntVector> {
 public:
  IntVectorBuilder &default_value(const int2 value);
  IntVectorBuilder &default_value(const int3 value);
  IntVectorBuilder &subtype(PropertySubType subtype);
  IntVectorBuilder &dimensions(int dimensions);
  IntVectorBuilder &min(int min);
  IntVectorBuilder &max(int max);
  IntVectorBuilder &compact();
};

class BoolBuilder;

class Bool : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_BOOLEAN;

  bool default_value = false;
  friend BoolBuilder;

  using Builder = BoolBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class BoolBuilder : public SocketDeclarationBuilder<Bool> {
 public:
  BoolBuilder &default_value(bool value);
  BoolBuilder &try_copy_ui_data(const SocketDeclaration &other_decl) override;
};

class ColorBuilder;

class Color : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_RGBA;

  ColorGeometry4f default_value{0.8f, 0.8f, 0.8f, 1.0f};

  friend ColorBuilder;

  using Builder = ColorBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class ColorBuilder : public SocketDeclarationBuilder<Color> {
 public:
  ColorBuilder &default_value(const ColorGeometry4f value);
  ColorBuilder &try_copy_ui_data(const SocketDeclaration &other_decl) override;
};

class Color3Builder;

class Color3 : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_RGB;

  ColorGeometry4f default_value{0.8f, 0.8f, 0.8f, 1.0f};

  friend Color3Builder;

  using Builder = Color3Builder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class Color3Builder : public SocketDeclarationBuilder<Color3> {
 public:
  Color3Builder &default_value(const ColorGeometry4f value);
  Color3Builder &try_copy_ui_data(const SocketDeclaration &other_decl) override;
};

class RotationBuilder;

class Rotation : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_ROTATION;

  math::EulerXYZ default_value;

  friend RotationBuilder;

  using Builder = RotationBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class RotationBuilder : public SocketDeclarationBuilder<Rotation> {
 public:
  RotationBuilder &default_value(const math::EulerXYZ &value);
  RotationBuilder &try_copy_ui_data(const SocketDeclaration &other_decl) override;
};

class Matrix3x3Builder;

class Matrix3x3 : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_MATRIX3X3;

  friend Matrix3x3Builder;

  using Builder = Matrix3x3Builder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class Matrix3x3Builder : public SocketDeclarationBuilder<Matrix3x3> {};

class MatrixBuilder;

class Matrix : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_MATRIX;

  friend MatrixBuilder;

  using Builder = MatrixBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class MatrixBuilder : public SocketDeclarationBuilder<Matrix> {};

class StringBuilder;

class String : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_STRING;

  std::string default_value;
  PropertySubType subtype = PROP_NONE;
  std::optional<std::string> path_filter;

  friend StringBuilder;

  using Builder = StringBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class StringBuilder : public SocketDeclarationBuilder<String> {
 public:
  StringBuilder &default_value(const std::string value);
  StringBuilder &subtype(PropertySubType subtype);
  StringBuilder &path_filter(std::optional<std::string> filter);
  StringBuilder &try_copy_ui_data(const SocketDeclaration &other_decl) override;
};

class MenuBuilder;

class Menu : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_MENU;

  MenuValue default_value;
  bool is_expanded = false;
  ImplicitSharingPtr<bke::RuntimeNodeEnumItems> items;

  friend MenuBuilder;

  using Builder = MenuBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class MenuBuilder : public SocketDeclarationBuilder<Menu> {
 public:
  MenuBuilder &default_value(MenuValue value);

  /** Draw the menu items next to each other instead of as a drop-down menu. */
  MenuBuilder &expanded(bool value = true);

  using StaticItemFilterFn = bool (*)(const EnumPropertyItem &item);
  /** Set the available items in the menu. The items array must have static lifetime. */
  MenuBuilder &static_items(const EnumPropertyItem *items,
                            const StaticItemFilterFn filter_fn = nullptr);
};

class BundleBuilder;

class Bundle : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_BUNDLE;
  /**
   * Index of a corresponding input socket. If set, the output is assumed to have the same bundle
   * structure as the input.
   */
  std::optional<int> pass_through_input_index;

  std::optional<BundleType> bundle_type;

  friend BundleBuilder;

  using Builder = BundleBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class BundleBuilder : public SocketDeclarationBuilder<Bundle> {
 public:
  BundleBuilder &bundle_type(BundleType bundle_type);

  /** On output sockets, indicate that the bundle structure is passed through from an input. */
  BundleBuilder &pass_through_input_index(std::optional<int> index);
};

class ClosureBuilder;

class Closure : public SocketDeclaration {
 public:
  std::unique_ptr<std::function<ClosureSignature(const bNode &)>> create_signature;

  Closure();
  ~Closure() override;

  static constexpr eNodeSocketDatatype static_socket_type = SOCK_CLOSURE;

  friend ClosureBuilder;

  using Builder = ClosureBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class ClosureBuilder : public SocketDeclarationBuilder<Closure> {
 public:
  void create_signature(std::function<ClosureSignature(const bNode &)> create_signature);
};

class IDSocketDeclaration : public SocketDeclaration {
 public:
  const char *idname;
  /**
   * Get the default ID pointer for this socket. This is a function to avoid dangling pointers,
   * since bNode::id pointers are remapped as ID pointers change, but pointers in socket
   * declarations are not managed the same way.
   */
  std::function<ID *(const bNode &node)> default_value_fn;

  IDSocketDeclaration(const char *idname);

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

template<typename T> class IDSocketDeclarationBuilder : public SocketDeclarationBuilder<T> {
 public:
  IDSocketDeclarationBuilder &default_value_fn(std::function<ID *(const bNode &node)> fn)
  {
    this->decl_->default_value_fn = std::move(fn);
    return *this;
  }
};

class Object : public IDSocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_OBJECT;

  using Builder = IDSocketDeclarationBuilder<Object>;

  Object();
};

class Material : public IDSocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_MATERIAL;

  using Builder = IDSocketDeclarationBuilder<Material>;

  Material();
};

class Collection : public IDSocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_COLLECTION;

  using Builder = IDSocketDeclarationBuilder<Collection>;

  Collection();
};

class Texture : public IDSocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_TEXTURE;

  using Builder = IDSocketDeclarationBuilder<Texture>;

  Texture();
};

class Image : public IDSocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_IMAGE;

  using Builder = IDSocketDeclarationBuilder<Image>;

  Image();
};

class Font : public IDSocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_FONT;

  using Builder = IDSocketDeclarationBuilder<Font>;

  Font();
};

class Scene : public IDSocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_SCENE;

  using Builder = IDSocketDeclarationBuilder<Scene>;

  Scene();
};

class Text : public IDSocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_TEXT_ID;

  using Builder = IDSocketDeclarationBuilder<Text>;

  Text();
};

class Mask : public IDSocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_MASK;

  using Builder = IDSocketDeclarationBuilder<Mask>;

  Mask();
};

class Sound : public IDSocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_SOUND;

  using Builder = IDSocketDeclarationBuilder<Sound>;

  Sound();
};

class ShaderBuilder;

class Shader : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_SHADER;

  friend ShaderBuilder;

  using Builder = ShaderBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class ShaderBuilder : public SocketDeclarationBuilder<Shader> {};

class SurfaceShaderBuilder;

class SurfaceShader : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_SURF_SHADER;

  friend SurfaceShaderBuilder;

  using Builder = SurfaceShaderBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class SurfaceShaderBuilder : public SocketDeclarationBuilder<SurfaceShader> {};

class VolumeShaderBuilder;

class VolumeShader : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_VOL_SHADER;

  friend VolumeShaderBuilder;

  using Builder = VolumeShaderBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class VolumeShaderBuilder : public SocketDeclarationBuilder<VolumeShader> {};

class DisplacementShaderBuilder;

class DisplacementShader : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_DISP_SHADER;

  friend DisplacementShaderBuilder;

  using Builder = DisplacementShaderBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class DisplacementShaderBuilder : public SocketDeclarationBuilder<DisplacementShader> {};

class LightShaderBuilder;

class LightShader : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_LIGHT_SHADER;

  friend LightShaderBuilder;

  using Builder = LightShaderBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class LightShaderBuilder : public SocketDeclarationBuilder<LightShader> {};

class BSDFBuilder;

class BSDF : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_BSDF;

  friend BSDFBuilder;

  using Builder = BSDFBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class BSDFBuilder : public SocketDeclarationBuilder<BSDF> {};

class EDFBuilder;

class EDF : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_EDF;

  friend EDFBuilder;

  using Builder = EDFBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class EDFBuilder : public SocketDeclarationBuilder<EDF> {};

class VDFBuilder;

class VDF : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_VDF;

  friend VDFBuilder;

  using Builder = VDFBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class VDFBuilder : public SocketDeclarationBuilder<VDF> {};

class ExtendBuilder;

class Extend : public SocketDeclaration {
 private:
  friend ExtendBuilder;

 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_CUSTOM;

  using Builder = ExtendBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class ExtendBuilder : public SocketDeclarationBuilder<Extend> {};

class FilenameBuilder;

class Filename : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_FILENAME;

  std::string default_value;
  PropertySubType subtype = PROP_FILEPATH;  // Sets up fallback file handling
  std::optional<std::string> path_filter;   // For extensions like .png, .abc

  friend FilenameBuilder;
  using Builder = FilenameBuilder;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class FilenameBuilder : public SocketDeclarationBuilder<Filename> {
 public:
  FilenameBuilder &default_value(const std::string value)
  {
    decl_->default_value = std::move(value);
    return *this;
  }
  FilenameBuilder &subtype(PropertySubType subtype)
  {
    decl_->subtype = subtype;
    return *this;
  }
  FilenameBuilder &path_filter(std::string filter)
  {
    decl_->path_filter = std::move(filter);
    return *this;
  }
  FilenameBuilder &try_copy_ui_data(const SocketDeclaration &other_decl) override;
};

class CustomTypeBuilder;

class Custom : public SocketDeclaration {
 public:
  static constexpr eNodeSocketDatatype static_socket_type = SOCK_CUSTOM;

  friend CustomTypeBuilder;

  using Builder = CustomTypeBuilder;

  const char *idname_;
  std::function<void(bNode &node, bNodeSocket &socket, const char *data_path)> init_socket_fn;

  bNodeSocket &build(bNodeTree &ntree, bNode &node) const override;
  bool matches(const bNodeSocket &socket) const override;
  bNodeSocket &update_or_build(bNodeTree &ntree, bNode &node, bNodeSocket &socket) const override;
  bool can_connect(const bNodeSocket &socket) const override;
};

class CustomTypeBuilder : public SocketDeclarationBuilder<Custom> {
 public:
  CustomTypeBuilder &idname(const char *idname);

  CustomTypeBuilder &init_socket_fn(
      std::function<void(bNode &node, bNodeSocket &socket, const char *data_path)> fn)
  {
    decl_->init_socket_fn = std::move(fn);
    return *this;
  }
};

/* -------------------------------------------------------------------- */
/** \name #FloatBuilder Inline Methods
 * \{ */

inline FloatBuilder &FloatBuilder::min(const float value)
{
  decl_->soft_min_value = value;
  return *this;
}

inline FloatBuilder &FloatBuilder::max(const float value)
{
  decl_->soft_max_value = value;
  return *this;
}

inline FloatBuilder &FloatBuilder::default_value(const float value)
{
  decl_->default_value = value;
  return *this;
}

inline FloatBuilder &FloatBuilder::subtype(PropertySubType subtype)
{
  decl_->subtype = subtype;
  return *this;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #IntBuilder Inline Methods
 * \{ */

inline IntBuilder &IntBuilder::min(const int value)
{
  decl_->soft_min_value = value;
  return *this;
}

inline IntBuilder &IntBuilder::max(const int value)
{
  decl_->soft_max_value = value;
  return *this;
}

inline IntBuilder &IntBuilder::default_value(const int value)
{
  decl_->default_value = value;
  return *this;
}

inline IntBuilder &IntBuilder::subtype(PropertySubType subtype)
{
  decl_->subtype = subtype;
  return *this;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #VectorBuilder Inline Methods
 * \{ */

inline VectorBuilder &VectorBuilder::default_value(const float2 value)
{
  decl_->default_value = float4(value, 0.0f, 0.0f);
  return *this;
}

inline VectorBuilder &VectorBuilder::default_value(const float3 value)
{
  decl_->default_value = float4(value, 0.0f);
  return *this;
}

inline VectorBuilder &VectorBuilder::default_value(const float4 value)
{
  decl_->default_value = value;
  return *this;
}

inline VectorBuilder &VectorBuilder::subtype(PropertySubType subtype)
{
  decl_->subtype = subtype;
  return *this;
}

inline VectorBuilder &VectorBuilder::dimensions(int dimensions)
{
  BLI_assert(dimensions >= 2 && dimensions <= 4);
  decl_->dimensions = dimensions;
  return *this;
}

inline VectorBuilder &VectorBuilder::min(const float min)
{
  decl_->soft_min_value = min;
  return *this;
}

inline VectorBuilder &VectorBuilder::max(const float max)
{
  decl_->soft_max_value = max;
  return *this;
}

inline VectorBuilder &VectorBuilder::compact()
{
  decl_->compact = true;
  return *this;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #Vector2Builder Inline Methods
 * \{ */

inline Vector2Builder &Vector2Builder::default_value(const float2 value)
{
  decl_->default_value = value;
  return *this;
}

inline Vector2Builder &Vector2Builder::subtype(PropertySubType subtype)
{
  decl_->subtype = subtype;
  return *this;
}

inline Vector2Builder &Vector2Builder::min(const float min)
{
  decl_->soft_min_value = min;
  return *this;
}

inline Vector2Builder &Vector2Builder::max(const float max)
{
  decl_->soft_max_value = max;
  return *this;
}

inline Vector2Builder &Vector2Builder::compact()
{
  decl_->compact = true;
  return *this;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #Vector4Builder Inline Methods
 * \{ */

inline Vector4Builder &Vector4Builder::default_value(const float4 value)
{
  decl_->default_value = value;
  return *this;
}

inline Vector4Builder &Vector4Builder::subtype(PropertySubType subtype)
{
  decl_->subtype = subtype;
  return *this;
}

inline Vector4Builder &Vector4Builder::min(const float min)
{
  decl_->soft_min_value = min;
  return *this;
}

inline Vector4Builder &Vector4Builder::max(const float max)
{
  decl_->soft_max_value = max;
  return *this;
}

inline Vector4Builder &Vector4Builder::compact()
{
  decl_->compact = true;
  return *this;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #IntVectorBuilder Inline Methods
 * \{ */

inline IntVectorBuilder &IntVectorBuilder::default_value(const int2 value)
{
  decl_->default_value = int3(value, 0);
  return *this;
}

inline IntVectorBuilder &IntVectorBuilder::default_value(const int3 value)
{
  decl_->default_value = value;
  return *this;
}

inline IntVectorBuilder &IntVectorBuilder::subtype(PropertySubType subtype)
{
  decl_->subtype = subtype;
  return *this;
}

inline IntVectorBuilder &IntVectorBuilder::dimensions(int dimensions)
{
  BLI_assert(dimensions >= 2 && dimensions <= 3);
  decl_->dimensions = dimensions;
  return *this;
}

inline IntVectorBuilder &IntVectorBuilder::min(const int min)
{
  decl_->soft_min_value = min;
  return *this;
}

inline IntVectorBuilder &IntVectorBuilder::max(const int max)
{
  decl_->soft_max_value = max;
  return *this;
}

inline IntVectorBuilder &IntVectorBuilder::compact()
{
  decl_->compact = true;
  return *this;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #BoolBuilder Inline Methods
 * \{ */

inline BoolBuilder &BoolBuilder::default_value(const bool value)
{
  decl_->default_value = value;
  return *this;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #ColorBuilder Inline Methods
 * \{ */

inline ColorBuilder &ColorBuilder::default_value(const ColorGeometry4f value)
{
  decl_->default_value = value;
  return *this;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #Color3Builder Inline Methods
 * \{ */

inline Color3Builder &Color3Builder::default_value(const ColorGeometry4f value)
{
  decl_->default_value = value;
  return *this;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #StringBuilder Inline Methods
 * \{ */

inline StringBuilder &StringBuilder::default_value(std::string value)
{
  decl_->default_value = std::move(value);
  return *this;
}

inline StringBuilder &StringBuilder::subtype(PropertySubType subtype)
{
  decl_->subtype = subtype;
  return *this;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #MenuBuilder Inline Methods
 * \{ */

inline MenuBuilder &MenuBuilder::default_value(const MenuValue value)
{
  decl_->default_value = value;
  return *this;
}

inline MenuBuilder &MenuBuilder::expanded(const bool value)
{
  decl_->is_expanded = value;
  return *this;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #RotationBuilder Inline Methods
 * \{ */

inline RotationBuilder &RotationBuilder::default_value(const math::EulerXYZ &value)
{
  decl_->default_value = value;
  return *this;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #IDSocketDeclaration and Children Inline Methods
 * \{ */

inline IDSocketDeclaration::IDSocketDeclaration(const char *idname) : idname(idname) {}

inline Object::Object() : IDSocketDeclaration("NodeSocketObject") {}

inline Material::Material() : IDSocketDeclaration("NodeSocketMaterial") {}

inline Collection::Collection() : IDSocketDeclaration("NodeSocketCollection") {}

inline Texture::Texture() : IDSocketDeclaration("NodeSocketTexture") {}

inline Image::Image() : IDSocketDeclaration("NodeSocketImage") {}

inline Font::Font() : IDSocketDeclaration("NodeSocketFont") {}

inline Scene::Scene() : IDSocketDeclaration("NodeSocketScene") {}

inline Text::Text() : IDSocketDeclaration("NodeSocketText") {}

inline Mask::Mask() : IDSocketDeclaration("NodeSocketMask") {}

inline Sound::Sound() : IDSocketDeclaration("NodeSocketSound") {}

/** \} */

/* -------------------------------------------------------------------- */
/** \name #CustomTypeBuilder Inline Methods
 * \{ */

inline CustomTypeBuilder &CustomTypeBuilder::idname(const char *idname)
{
  decl_->idname_ = idname;
  return *this;
}

/** \} */

}  // namespace blender::nodes::decl
