/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "abc_writer_attribute.h"

#include "DNA_meshdata_types.h"  // For MStringProperty

#include "BLI_color.hh"
#include "BLI_math_quaternion_types.hh"
#include "BLI_math_vector_types.hh"

#include "BKE_attribute.hh"

#include "intern/abc_axis_conversion.h"
#include "intern/abc_util.h"

#include "CLG_log.h"

using namespace Alembic::AbcGeom;

static CLG_LogRef LOG = {"io.alembic"};

namespace blender::io::alembic {

template<typename BlenderDataType> struct TypeTraitConverter;

template<> struct TypeTraitConverter<bool> {
  using AbcTraitType = Alembic::Abc::BooleanTPTraits;

  static Alembic::Abc::bool_t convert(bool v)
  {
    return v;
  }
};

template<> struct TypeTraitConverter<int8_t> {
  using AbcTraitType = Alembic::Abc::Int8TPTraits;

  static int8_t convert(int8_t v)
  {
    return static_cast<int8_t>(v);
  }
};

template<> struct TypeTraitConverter<short2> {
  using AbcTraitType = Alembic::Abc::V2sTPTraits;

  static Imath::V2s convert(short2 v)
  {
    return {v.x, v.y};
  }
};

template<> struct TypeTraitConverter<int> {
  using AbcTraitType = Alembic::Abc::Int32TPTraits;

  static int convert(int v)
  {
    return v;
  }
};

template<> struct TypeTraitConverter<int2> {
  using AbcTraitType = Alembic::Abc::V2iTPTraits;

  static Imath::V2i convert(int2 v)
  {
    return {v.x, v.y};
  }
};

template<> struct TypeTraitConverter<float> {
  using AbcTraitType = Alembic::Abc::Float32TPTraits;

  static float convert(float v)
  {
    return v;
  }
};

template<> struct TypeTraitConverter<float2> {
  using AbcTraitType = Alembic::Abc::V2fTPTraits;

  static Imath::V2f convert(float2 v)
  {
    return {v.x, v.y};
  }
};

template<> struct TypeTraitConverter<float3> {
  using AbcTraitType = Alembic::Abc::V3fTPTraits;

  static Imath::V3f convert(float3 v)
  {
    copy_yup_from_zup(v, v);
    return {v.x, v.y, v.z};
  }
};

template<> struct TypeTraitConverter<float4x4> {
  /* Although matrices are stored as floats in Blender, we convert to double as this is what most
   * other software use. */
  using AbcTraitType = Alembic::Abc::M44dTPTraits;

  static Imath::M44d convert(float4x4 v)
  {
    copy_m44_axis_swap(v.ptr(), v.ptr(), ABC_YUP_FROM_ZUP);
    return convert_matrix_datatype(v.ptr());
  }
};

template<> struct TypeTraitConverter<ColorGeometry4f> {
  using AbcTraitType = Alembic::Abc::C4fTPTraits;

  static Imath::C4f convert(ColorGeometry4f v)
  {
    return {v.r, v.g, v.b, v.a};
  }
};

template<> struct TypeTraitConverter<ColorGeometry4b> {
  using AbcTraitType = Alembic::Abc::C4cTPTraits;

  static Imath::C4c convert(ColorGeometry4b v)
  {
    return {v.r, v.g, v.b, v.a};
  }
};

template<> struct TypeTraitConverter<math::Quaternion> {
  using AbcTraitType = Alembic::Abc::QuatfTPTraits;

  static Imath::Quatf convert(math::Quaternion quat)
  {
    float3 v = quat.imaginary_part();
    copy_yup_from_zup(v, v);
    return {quat.w, v.x, v.y, v.z};
  }
};

template<> struct TypeTraitConverter<MStringProperty> {
  using AbcTraitType = Alembic::Abc::StringTPTraits;

  static std::string convert(const MStringProperty &v)
  {
    return std::string(v.s, size_t(v.s_len));
  }
};

static GeometryScope get_scope_for_attribute_domain(bke::AttrDomain domain)
{
  switch (domain) {
    case bke::AttrDomain::Instance:
    case bke::AttrDomain::Edge:
    case bke::AttrDomain::Layer: {
      /* Not supported */
      break;
    }
    case bke::AttrDomain::Point: {
      return kVertexScope;
    }
    case bke::AttrDomain::Curve:
    case bke::AttrDomain::Face: {
      return kUniformScope;
    }
    case bke::AttrDomain::Corner: {
      return kFacevaryingScope;
    }
  }

  return kUnknownScope;
}

void AttributeParamMaps::write_empty_samples(const size_t num_geom_samples)
{
#define WRITE_EMPTY_SAMPLE(abc_type, map_name) \
  for (auto &iter : this->map_name) { \
    for (size_t i = iter.second.getNumSamples(); i < num_geom_samples; i++) { \
      abc_type::Sample sample; \
      sample.setVals({}); \
      iter.second.set(sample); \
    } \
  }
  ENUMERATE_EXPORTED_TYPES(WRITE_EMPTY_SAMPLE)
#undef WRITE_EMPTY_SAMPLE
}

template<typename BlenderDataType>
static void create_geom_param_for_attribute(const Alembic::Abc::OCompoundProperty &prop,
                                            AttributeParamMaps &param_maps,
                                            const VArray<BlenderDataType> &buffer,
                                            const StringRefNull name,
                                            const GeometryScope scope,
                                            const int timesample_index,
                                            const OffsetIndices<int> corner_indices,
                                            const StringRefNull object_name)
{
  using TypeConverter = TypeTraitConverter<BlenderDataType>;
  using AlembicTraitType = typename TypeConverter::AbcTraitType;
  using AlembicType = typename AlembicTraitType::value_type;

  std::vector<AlembicType> values;

  GeometryScope corrected_scope = scope;

  if (buffer.is_single()) {
    corrected_scope = GeometryScope::kConstantScope;
    values.push_back(TypeConverter::convert(buffer[0]));
  }
  else {
    values.resize(static_cast<size_t>(buffer.size()));
    AlembicType *values_ptr = values.data();
    if (scope == kFacevaryingScope) {
      /* NOTE: data needs to be written in the reverse order. */
      for (const int i : corner_indices.index_range()) {
        const IndexRange corners = corner_indices[i];
        for (int j = corners.size() - 1; j >= 0; j--) {
          const int blender_index = corners[j];
          *values_ptr++ = TypeConverter::convert(buffer[blender_index]);
        }
      }
    }
    else {
      for (int i = 0; i < buffer.size(); i++) {
        *values_ptr++ = TypeConverter::convert(buffer[i]);
      }
    }
  }

  using ParamType = OTypedGeomParam<AlembicTraitType>;
  using SampleType = typename ParamType::Sample;

  try {
    ParamType param;
    param_maps.ensure_param(prop, param, name, corrected_scope);
    param.setTimeSampling(timesample_index);

    SampleType sample(values, corrected_scope);
    param.set(sample);
  }
  catch (const Alembic::Util::Exception &ex) {
    CLOG_WARN(&LOG,
              "On object '%s', error writing attribute '%s' : %s",
              object_name.c_str(),
              name.c_str(),
              ex.what());
  }
}

void create_geom_param_for_attribute(const Alembic::Abc::OCompoundProperty &prop,
                                     AttributeParamMaps &param_maps,
                                     const bke::AttributeIter &attr,
                                     const int timesample_index,
                                     const OffsetIndices<int> corner_indices,
                                     const StringRefNull object_name)
{
  const GeometryScope scope = get_scope_for_attribute_domain(attr.domain);
  if (scope == kUnknownScope) {
    CLOG_WARN(&LOG,
              "On object '%s', attribute '%s' (Blender domain %d, type %d) cannot be converted "
              "to Alembic",
              object_name.c_str(),
              attr.name.c_str(),
              int8_t(attr.domain),
              int(attr.data_type));
    return;
  }

  const GVArray attribute = *attr.get();

  switch (attr.data_type) {
    case bke::AttrType::Bool:
      create_geom_param_for_attribute(prop,
                                      param_maps,
                                      attribute.typed<bool>(),
                                      attr.name,
                                      scope,
                                      timesample_index,
                                      corner_indices,
                                      object_name);
      break;
    case bke::AttrType::Int8:
      create_geom_param_for_attribute(prop,
                                      param_maps,
                                      attribute.typed<int8_t>(),
                                      attr.name,
                                      scope,
                                      timesample_index,
                                      corner_indices,
                                      object_name);
      break;
    case bke::AttrType::Int16_2D:
      create_geom_param_for_attribute(prop,
                                      param_maps,
                                      attribute.typed<short2>(),
                                      attr.name,
                                      scope,
                                      timesample_index,
                                      corner_indices,
                                      object_name);
      break;
    case bke::AttrType::Int32:
      create_geom_param_for_attribute(prop,
                                      param_maps,
                                      attribute.typed<int>(),
                                      attr.name,
                                      scope,
                                      timesample_index,
                                      corner_indices,
                                      object_name);
      break;
    case bke::AttrType::Int32_2D:
      create_geom_param_for_attribute(prop,
                                      param_maps,
                                      attribute.typed<int2>(),
                                      attr.name,
                                      scope,
                                      timesample_index,
                                      corner_indices,
                                      object_name);
      break;
    case bke::AttrType::ColorByte:
      create_geom_param_for_attribute(prop,
                                      param_maps,
                                      attribute.typed<ColorGeometry4b>(),
                                      attr.name,
                                      scope,
                                      timesample_index,
                                      corner_indices,
                                      object_name);
      break;
    case bke::AttrType::ColorFloat:
      create_geom_param_for_attribute(prop,
                                      param_maps,
                                      attribute.typed<ColorGeometry4f>(),
                                      attr.name,
                                      scope,
                                      timesample_index,
                                      corner_indices,
                                      object_name);
      break;
    case bke::AttrType::Float:
      create_geom_param_for_attribute(prop,
                                      param_maps,
                                      attribute.typed<float>(),
                                      attr.name,
                                      scope,
                                      timesample_index,
                                      corner_indices,
                                      object_name);
      break;
    case bke::AttrType::Float2:
      create_geom_param_for_attribute(prop,
                                      param_maps,
                                      attribute.typed<float2>(),
                                      attr.name,
                                      scope,
                                      timesample_index,
                                      corner_indices,
                                      object_name);
      break;
    case bke::AttrType::Float3:
      create_geom_param_for_attribute(prop,
                                      param_maps,
                                      attribute.typed<float3>(),
                                      attr.name,
                                      scope,
                                      timesample_index,
                                      corner_indices,
                                      object_name);
      break;
    case bke::AttrType::Float4:
      /* Alembic does not have an equivalent to float4. If needed we could either flatten it to a
       * float, export it as a float2 with an array extent of 2, or even export as quaternion or
       * box2. */
      CLOG_WARN(&LOG,
                "On object '%s', attribute '%s' of type %d is not supported by Alembic",
                object_name.c_str(),
                attr.name.c_str(),
                int(attr.data_type));
      break;
    case bke::AttrType::Quaternion:
      create_geom_param_for_attribute(prop,
                                      param_maps,
                                      attribute.typed<math::Quaternion>(),
                                      attr.name,
                                      scope,
                                      timesample_index,
                                      corner_indices,
                                      object_name);
      break;
    case bke::AttrType::Float4x4:
      create_geom_param_for_attribute(prop,
                                      param_maps,
                                      attribute.typed<float4x4>(),
                                      attr.name,
                                      scope,
                                      timesample_index,
                                      corner_indices,
                                      object_name);
      break;
    case bke::AttrType::String:
      create_geom_param_for_attribute(prop,
                                      param_maps,
                                      attribute.typed<MStringProperty>(),
                                      attr.name,
                                      scope,
                                      timesample_index,
                                      corner_indices,
                                      object_name);
      break;
    default:
      break;
  }
}

}  // namespace blender::io::alembic
