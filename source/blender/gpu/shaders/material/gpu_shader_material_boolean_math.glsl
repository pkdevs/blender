/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

[[node]]
void boolean_math_and(bool a, bool b, out bool result)
{
  result = (a && b);
}

[[node]]
void boolean_math_or(bool a, bool b, out bool result)
{
  result = (a || b);
}

[[node]]
void boolean_math_not(bool a, out bool result)
{
  result = !a;
}

[[node]]
void boolean_math_nand(bool a, bool b, out bool result)
{
  result = !(a && b);
}

[[node]]
void boolean_math_nor(bool a, bool b, out bool result)
{
  result = !(a || b);
}

[[node]]
void boolean_math_xnor(bool a, bool b, out bool result)
{
  result = (a == b);
}

[[node]]
void boolean_math_xor(bool a, bool b, out bool result)
{
  result = (a != b);
}

[[node]]
void boolean_math_imply(bool a, bool b, out bool result)
{
  result = (!a || b);
}

[[node]]
void boolean_math_nimply(bool a, bool b, out bool result)
{
  result = (a && !b);
}

/* EEVEE represents boolean sockets as floats. */

[[node]]
void boolean_math_and_float(float a, float b, out float result)
{
  result = (a != 0.0f && b != 0.0f) ? 1.0f : 0.0f;
}

[[node]]
void boolean_math_or_float(float a, float b, out float result)
{
  result = (a != 0.0f || b != 0.0f) ? 1.0f : 0.0f;
}

[[node]]
void boolean_math_not_float(float a, out float result)
{
  result = (a == 0.0f) ? 1.0f : 0.0f;
}

[[node]]
void boolean_math_nand_float(float a, float b, out float result)
{
  result = (a != 0.0f && b != 0.0f) ? 0.0f : 1.0f;
}

[[node]]
void boolean_math_nor_float(float a, float b, out float result)
{
  result = (a != 0.0f || b != 0.0f) ? 0.0f : 1.0f;
}

[[node]]
void boolean_math_xnor_float(float a, float b, out float result)
{
  result = ((a != 0.0f) == (b != 0.0f)) ? 1.0f : 0.0f;
}

[[node]]
void boolean_math_xor_float(float a, float b, out float result)
{
  result = ((a != 0.0f) != (b != 0.0f)) ? 1.0f : 0.0f;
}

[[node]]
void boolean_math_imply_float(float a, float b, out float result)
{
  result = (a == 0.0f || b != 0.0f) ? 1.0f : 0.0f;
}

[[node]]
void boolean_math_nimply_float(float a, float b, out float result)
{
  result = (a != 0.0f && b == 0.0f) ? 1.0f : 0.0f;
}
