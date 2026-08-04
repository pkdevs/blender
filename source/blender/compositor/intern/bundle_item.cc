/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "BLI_string_ref.hh"

#include "COM_bundle_item.hh"
#include "COM_context.hh"
#include "COM_result.hh"

namespace blender::compositor {

BundleItem::BundleItem(Context &context, Result &result)
    : result_(context.create_result(result.type(), result.precision()))
{
  result_.share_data(result);
}

nodes::BundleItemValue BundleItem::NewBundleItemValue(Context &context, Result &result)
{
  return nodes::BundleItemValue{nodes::BundleItemInternalValue{
      ImplicitSharingPtr<const BundleItem>(new BundleItem(context, result))}};
}

StringRefNull BundleItem::type_name() const
{
  return "Compositor Bundle Item";
}

const Result &BundleItem::result() const
{
  return result_;
}

void BundleItem::delete_self()
{
  result_.release();
  delete this;
}

}  // namespace blender::compositor
