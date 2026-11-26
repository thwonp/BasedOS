#!/usr/bin/env python3
"""The rotation layer's two shaders, as SPIR-V 1.0, written out as a C header (spirv.h).

Run it again after changing a shader here: python3 gen-spirv.py > spirv.h
There is no shader compiler in BasedOS's build (no host glslang or SPIRV-Tools), so the shaders are
assembled here, one instruction at a time. In GLSL they are:

  vertex:   void main() {
                vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
                gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);      // one triangle over the screen
            }

  fragment: layout(set = 0, binding = 0) uniform texture2D src;     // the app's frame
            layout(push_constant) uniform PC { ivec4 x; ivec4 y; } pc;
            layout(location = 0) out vec4 color;
            void main() {
                ivec2 d = ivec2(gl_FragCoord.xy);                   // the panel's pixel
                ivec2 s = ivec2(pc.x.x + pc.x.y * d.x + pc.x.z * d.y,
                                pc.y.x + pc.y.y * d.x + pc.y.z * d.y);
                color = texelFetch(src, s, 0);                      // the app's pixel that goes there
            }
"""
import struct
import sys

# Opcodes, and the enum values used (SPIR-V 1.0 specification).
OP = dict(Capability=17, MemoryModel=14, EntryPoint=15, ExecutionMode=16, Decorate=71,
          MemberDecorate=72, TypeVoid=19, TypeInt=21, TypeFloat=22, TypeVector=23, TypeImage=25,
          TypeStruct=30, TypePointer=32, TypeFunction=33, Constant=43, Variable=59, Load=61,
          Store=62, AccessChain=65, Function=54, FunctionEnd=56, Label=248, Return=253,
          CompositeConstruct=80, CompositeExtract=81, ConvertFToS=110, ConvertSToF=111, IAdd=128,
          IMul=132, FMul=133, FSub=131, ShiftLeftLogical=196, BitwiseAnd=199, ImageFetch=95)
CAP_SHADER = 1
ADDR_LOGICAL, MEM_GLSL450 = 0, 1
MODEL_VERTEX, MODEL_FRAGMENT = 0, 4
MODE_ORIGIN_UPPER_LEFT = 7
DEC_BLOCK, DEC_BUILTIN, DEC_LOCATION, DEC_BINDING, DEC_SET, DEC_OFFSET = 2, 11, 30, 33, 34, 35
BUILTIN_POSITION, BUILTIN_FRAGCOORD, BUILTIN_VERTEXINDEX = 0, 15, 42
SC_UNIFORMCONST, SC_INPUT, SC_OUTPUT, SC_PUSHCONST = 0, 1, 3, 9
DIM_2D = 1
IMAGE_OPERAND_LOD = 2


def f32(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def string(s):
    b = s.encode() + b"\0"
    b += b"\0" * (-len(b) % 4)
    return list(struct.unpack("<%dI" % (len(b) // 4), b))


class Module:
    def __init__(self):
        self.words = []
        self.next_id = 1

    def id(self):
        i = self.next_id
        self.next_id += 1
        return i

    def op(self, name, *operands):
        flat = []
        for o in operands:
            flat += o if isinstance(o, list) else [o]
        self.words += [(len(flat) + 1) << 16 | OP[name]] + flat

    def binary(self):
        return [0x07230203, 0x00010000, 0, self.next_id, 0] + self.words


def vertex():
    m = Module()
    (void, fn, int_, float_, v4f, pv, p_out_pv, p_out_v4f, p_in_int, c0, c1, c2, f0, f1, f2, var_pv,
     var_vi, main) = (m.id() for _ in range(18))
    m.op("Capability", CAP_SHADER)
    m.op("MemoryModel", ADDR_LOGICAL, MEM_GLSL450)
    m.op("EntryPoint", MODEL_VERTEX, main, string("main"), var_vi, var_pv)
    m.op("Decorate", pv, DEC_BLOCK)
    m.op("MemberDecorate", pv, 0, DEC_BUILTIN, BUILTIN_POSITION)
    m.op("Decorate", var_vi, DEC_BUILTIN, BUILTIN_VERTEXINDEX)
    m.op("TypeVoid", void)
    m.op("TypeFunction", fn, void)
    m.op("TypeInt", int_, 32, 1)
    m.op("TypeFloat", float_, 32)
    m.op("TypeVector", v4f, float_, 4)
    m.op("TypeStruct", pv, v4f)
    m.op("TypePointer", p_out_pv, SC_OUTPUT, pv)
    m.op("TypePointer", p_out_v4f, SC_OUTPUT, v4f)
    m.op("TypePointer", p_in_int, SC_INPUT, int_)
    m.op("Constant", int_, c0, 0)
    m.op("Constant", int_, c1, 1)
    m.op("Constant", int_, c2, 2)
    m.op("Constant", float_, f0, f32(0.0))
    m.op("Constant", float_, f1, f32(1.0))
    m.op("Constant", float_, f2, f32(2.0))
    m.op("Variable", p_out_pv, var_pv, SC_OUTPUT)
    m.op("Variable", p_in_int, var_vi, SC_INPUT)
    m.op("Function", void, main, 0, fn)
    m.op("Label", m.id())
    idx, sh, a, b, fa, fb, xa, xb, x, y, pos, ptr = (m.id() for _ in range(12))
    m.op("Load", int_, idx, var_vi)
    m.op("ShiftLeftLogical", int_, sh, idx, c1)
    m.op("BitwiseAnd", int_, a, sh, c2)
    m.op("BitwiseAnd", int_, b, idx, c2)
    m.op("ConvertSToF", float_, fa, a)
    m.op("ConvertSToF", float_, fb, b)
    m.op("FMul", float_, xa, fa, f2)
    m.op("FMul", float_, xb, fb, f2)
    m.op("FSub", float_, x, xa, f1)
    m.op("FSub", float_, y, xb, f1)
    m.op("CompositeConstruct", v4f, pos, x, y, f0, f1)
    m.op("AccessChain", p_out_v4f, ptr, var_pv, c0)
    m.op("Store", ptr, pos)
    m.op("Return")
    m.op("FunctionEnd")
    return m.binary()


def fragment():
    m = Module()
    (void, fn, int_, float_, v4f, v2i, v4i, img, pc, p_pc, p_pc_v4i, p_img, p_in_v4f, p_out_v4f, c0,
     c1, var_pc, var_src, var_fc, var_out, main) = (m.id() for _ in range(21))
    m.op("Capability", CAP_SHADER)
    m.op("MemoryModel", ADDR_LOGICAL, MEM_GLSL450)
    m.op("EntryPoint", MODEL_FRAGMENT, main, string("main"), var_fc, var_out)
    m.op("ExecutionMode", main, MODE_ORIGIN_UPPER_LEFT)
    m.op("Decorate", pc, DEC_BLOCK)
    m.op("MemberDecorate", pc, 0, DEC_OFFSET, 0)
    m.op("MemberDecorate", pc, 1, DEC_OFFSET, 16)
    m.op("Decorate", var_src, DEC_SET, 0)
    m.op("Decorate", var_src, DEC_BINDING, 0)
    m.op("Decorate", var_fc, DEC_BUILTIN, BUILTIN_FRAGCOORD)
    m.op("Decorate", var_out, DEC_LOCATION, 0)
    m.op("TypeVoid", void)
    m.op("TypeFunction", fn, void)
    m.op("TypeInt", int_, 32, 1)
    m.op("TypeFloat", float_, 32)
    m.op("TypeVector", v4f, float_, 4)
    m.op("TypeVector", v2i, int_, 2)
    m.op("TypeVector", v4i, int_, 4)
    # 2D, not depth, not arrayed, single-sampled, sampled (used with a sampler or fetched), any format
    m.op("TypeImage", img, float_, DIM_2D, 0, 0, 0, 1, 0)
    m.op("TypeStruct", pc, v4i, v4i)
    m.op("TypePointer", p_pc, SC_PUSHCONST, pc)
    m.op("TypePointer", p_pc_v4i, SC_PUSHCONST, v4i)
    m.op("TypePointer", p_img, SC_UNIFORMCONST, img)
    m.op("TypePointer", p_in_v4f, SC_INPUT, v4f)
    m.op("TypePointer", p_out_v4f, SC_OUTPUT, v4f)
    m.op("Constant", int_, c0, 0)
    m.op("Constant", int_, c1, 1)
    m.op("Variable", p_pc, var_pc, SC_PUSHCONST)
    m.op("Variable", p_img, var_src, SC_UNIFORMCONST)
    m.op("Variable", p_in_v4f, var_fc, SC_INPUT)
    m.op("Variable", p_out_v4f, var_out, SC_OUTPUT)
    m.op("Function", void, main, 0, fn)
    m.op("Label", m.id())
    fc, fx, fy, dx, dy = (m.id() for _ in range(5))
    m.op("Load", v4f, fc, var_fc)
    m.op("CompositeExtract", float_, fx, fc, 0)
    m.op("CompositeExtract", float_, fy, fc, 1)
    m.op("ConvertFToS", int_, dx, fx)
    m.op("ConvertFToS", int_, dy, fy)
    coords = []
    for member in (c0, c1):
        ptr, v, k, a, b, ax, by, s0, s1 = (m.id() for _ in range(9))
        m.op("AccessChain", p_pc_v4i, ptr, var_pc, member)
        m.op("Load", v4i, v, ptr)
        m.op("CompositeExtract", int_, k, v, 0)
        m.op("CompositeExtract", int_, a, v, 1)
        m.op("CompositeExtract", int_, b, v, 2)
        m.op("IMul", int_, ax, a, dx)
        m.op("IMul", int_, by, b, dy)
        m.op("IAdd", int_, s0, k, ax)
        m.op("IAdd", int_, s1, s0, by)
        coords.append(s1)
    s, image, texel = m.id(), m.id(), m.id()
    m.op("CompositeConstruct", v2i, s, coords[0], coords[1])
    m.op("Load", img, image, var_src)
    m.op("ImageFetch", v4f, texel, image, s, IMAGE_OPERAND_LOD, c0)
    m.op("Store", var_out, texel)
    m.op("Return")
    m.op("FunctionEnd")
    return m.binary()


def c_array(name, words):
    lines = ["static const uint32_t %s[%d] = {" % (name, len(words))]
    for i in range(0, len(words), 6):
        lines.append("\t" + ", ".join("0x%08x" % w for w in words[i:i + 6]) + ",")
    lines.append("};")
    return "\n".join(lines)


out = sys.stdout
out.write("/* Made by gen-spirv.py; don't edit. The rotation layer's shaders, as SPIR-V 1.0. */\n")
out.write("#include <stdint.h>\n\n")
out.write(c_array("rotate_vert_spv", vertex()) + "\n\n")
out.write(c_array("rotate_frag_spv", fragment()) + "\n")
