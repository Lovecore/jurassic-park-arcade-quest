#!/usr/bin/env python3
"""Generate host thunks for OpenGL from the Khronos registry (gl.xml).

Each thunk reads its arguments from the guest i386 stack (cdecl: [esp] is
the return address), converts guest pointers to host pointers (RT_MEM +
address; 0 stays NULL), calls the host GL entry point, puts the result in
eax (edx:eax for 64-bit) and pops the return address.

Pointer arguments that are buffer offsets when a buffer object is bound
(vertex/attrib pointers, element indices, pixel pack/unpack) are passed
through untranslated in that case; the thunks track the relevant bindings.
Commands whose arguments cannot be marshalled generically (arrays of
pointers, returned pointers, sync objects, callbacks) are left to hand-
written code in rt/hostlibs.c and listed in the output header.

  python tools/recomp/gen_gl.py ext/khronos/gl.xml -o tools/recomp/out/gl_thunks.c
"""
import argparse, re, xml.etree.ElementTree as ET

INT32 = {"GLenum", "GLboolean", "GLbitfield", "GLbyte", "GLshort", "GLint", "GLsizei", "GLubyte",
         "GLushort", "GLuint", "GLclampx", "GLfixed", "GLhalfNV", "GLhandleARB", "GLintptr",
         "GLsizeiptr", "GLintptrARB", "GLsizeiptrARB", "GLvdpauSurfaceNV", "GLchar", "GLcharARB",
         "GLhalf", "GLhalfARB"}
SIGNED = {"GLbyte", "GLshort", "GLint", "GLsizei", "GLfixed", "GLclampx", "GLintptr", "GLsizeiptr",
          "GLintptrARB", "GLsizeiptrARB"}
INT64 = {"GLint64", "GLuint64", "GLint64EXT", "GLuint64EXT"}
FLOAT = {"GLfloat", "GLclampf"}
DOUBLE = {"GLdouble", "GLclampd"}
# pointer args that are offsets when a buffer is bound: command -> (param, binding target)
OFFSET_PTR = {
    "glVertexPointer": ("pointer", "ARRAY"), "glNormalPointer": ("pointer", "ARRAY"),
    "glColorPointer": ("pointer", "ARRAY"), "glTexCoordPointer": ("pointer", "ARRAY"),
    "glIndexPointer": ("pointer", "ARRAY"), "glEdgeFlagPointer": ("pointer", "ARRAY"),
    "glFogCoordPointer": ("pointer", "ARRAY"), "glSecondaryColorPointer": ("pointer", "ARRAY"),
    "glVertexAttribPointer": ("pointer", "ARRAY"), "glVertexAttribPointerARB": ("pointer", "ARRAY"),
    "glVertexAttribIPointer": ("pointer", "ARRAY"), "glInterleavedArrays": ("pointer", "ARRAY"),
    "glDrawElements": ("indices", "ELEMENT"), "glDrawRangeElements": ("indices", "ELEMENT"),
    "glDrawRangeElementsEXT": ("indices", "ELEMENT"), "glDrawElementsInstanced": ("indices", "ELEMENT"),
    "glDrawElementsInstancedARB": ("indices", "ELEMENT"), "glDrawElementsBaseVertex": ("indices", "ELEMENT"),
    "glTexImage1D": ("pixels", "UNPACK"), "glTexImage2D": ("pixels", "UNPACK"), "glTexImage3D": ("pixels", "UNPACK"),
    "glTexSubImage1D": ("pixels", "UNPACK"), "glTexSubImage2D": ("pixels", "UNPACK"),
    "glTexSubImage3D": ("pixels", "UNPACK"), "glCompressedTexImage2D": ("data", "UNPACK"),
    "glCompressedTexImage2DARB": ("data", "UNPACK"), "glCompressedTexSubImage2D": ("data", "UNPACK"),
    "glCompressedTexImage3D": ("data", "UNPACK"), "glCompressedTexImage1D": ("data", "UNPACK"),
    "glDrawPixels": ("pixels", "UNPACK"), "glReadPixels": ("pixels", "PACK"), "glGetTexImage": ("pixels", "PACK"),
    "glGetCompressedTexImage": ("img", "PACK"), "glGetCompressedTexImageARB": ("img", "PACK"),
}
MANUAL = {"glGetString", "glGetStringi", "glMapBuffer", "glMapBufferARB", "glMapBufferRange",
          "glUnmapBuffer", "glUnmapBufferARB", "glShaderSource", "glShaderSourceARB",
          "glGetPointerv", "glGetVertexAttribPointerv", "glGetVertexAttribPointervARB",
          "glFenceSync", "glDeleteSync", "glClientWaitSync", "glWaitSync", "glGetSynciv", "glIsSync",
          "glDebugMessageCallback", "glDebugMessageCallbackARB", "glTransformFeedbackVaryings",
          "glMultiDrawElements", "glGetUniformIndices", "glCreateShaderProgramv",
          "glBindBuffer", "glBindBufferARB"}


def ctype_of(param):
    """(kind, host C type) of a <param>; kind: i32 u32 i64 f32 f64 ptr unsupported"""
    text = "".join(param.itertext())
    name = param.find("name").text
    decl = text[: text.rfind(name)]
    pt = param.find("ptype")
    ptype = pt.text if pt is not None else None
    if "*" in decl:
        if decl.count("*") > 1:
            return "unsupported", decl.strip()
        return "ptr", decl.strip()
    if ptype in INT64:
        return "i64", ptype
    if ptype in FLOAT:
        return "f32", ptype
    if ptype in DOUBLE:
        return "f64", ptype
    if ptype in INT32:
        return ("i32" if ptype in SIGNED else "u32"), ptype
    if ptype in ("GLsync", "GLDEBUGPROC", "GLDEBUGPROCARB", "GLDEBUGPROCAMD", "GLeglImageOES",
                 "GLVULKANPROCNV", "GLeglClientBufferEXT", "struct _cl_context", "struct _cl_event"):
        return "unsupported", ptype
    if ptype is None and decl.strip() in ("void",):
        return "unsupported", "void"
    return "unsupported", str(ptype)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("xml")
    ap.add_argument("-o", required=True)
    args = ap.parse_args()
    root = ET.parse(args.xml).getroot()
    out = []
    names = []
    skipped = []
    for cmd in root.find("commands").findall("command"):
        proto = cmd.find("proto")
        name = proto.find("name").text
        ptext = "".join(proto.itertext())
        rdecl = ptext[: ptext.rfind(name)].strip()
        rpt = proto.find("ptype")
        rtype = rpt.text if rpt is not None else rdecl
        if name in MANUAL:
            continue
        params = cmd.findall("param")
        kinds = [ctype_of(p) for p in params]
        pnames = [p.find("name").text for p in params]
        if any(k == "unsupported" for k, t in kinds):
            skipped.append(name)
            continue
        if "*" in rdecl:
            skipped.append(name)        # returns a pointer: hand-written
            continue
        if rtype in ("GLsync",):
            skipped.append(name)
            continue
        # read args
        lines = [f"static int glt_{name}(rt_cpu *cpu)", "{", "    uint32_t sp = cpu->esp + 4;"]
        cargs, ctypes = [], []
        off = OFFSET_PTR.get(name)
        for (k, t), pn in zip(kinds, pnames):
            v = f"a_{pn}"
            if k in ("i32", "u32"):
                # narrow types arrive promoted in a 4-byte slot with undefined upper bits
                cast = {"GLboolean": "(uint8_t)", "GLubyte": "(uint8_t)", "GLchar": "(uint8_t)",
                        "GLcharARB": "(uint8_t)", "GLbyte": "(int8_t)", "GLshort": "(int16_t)",
                        "GLushort": "(uint16_t)", "GLhalf": "(uint16_t)", "GLhalfARB": "(uint16_t)",
                        "GLhalfNV": "(uint16_t)"}.get(t, "")
                ct = "int32_t" if k == "i32" else "uint32_t"
                lines.append(f"    {ct} {v} = ({ct}){cast}rd32(sp); sp += 4;")
                ctypes.append(ct)
            elif k == "i64":
                lines.append(f"    uint64_t {v} = rd64(sp); sp += 8;")
                ctypes.append("uint64_t")
            elif k == "f32":
                lines.append(f"    float {v} = rdf32(sp); sp += 4;")
                ctypes.append("float")
            elif k == "f64":
                lines.append(f"    double {v} = rdf64(sp); sp += 8;")
                ctypes.append("double")
            else:
                if off and off[0] == pn:
                    lines.append(f"    uint32_t g_{pn} = rd32(sp); sp += 4;")
                    lines.append(f"    void *{v} = gl_bound_{off[1]}() ? (void *)(uintptr_t)g_{pn} "
                                 f": GUEST_PTR(g_{pn});")
                else:
                    lines.append(f"    void *{v} = GUEST_PTR(rd32(sp)); sp += 4;")
                ctypes.append("void *")
            cargs.append(v)
        lines.insert(3, "    (void)sp;")
        if rtype in INT64:
            rt = "uint64_t"
        elif rtype in FLOAT:
            rt = "float"
        elif rtype in DOUBLE:
            rt = "double"
        elif rtype == "void" or rtype is None or rtype == "":
            rt = "void"
        else:
            rt = "uint32_t"
        sig = ", ".join(ctypes) or "void"
        fp = f"(({rt} (GLAPI_CALL *)({sig}))gl_proc({len(names)}, \"{name}\"))"
        call = f"{fp}({', '.join(cargs)})"
        if rt == "void":
            lines.append(f"    {call};")
        elif rt == "uint64_t":
            lines.append(f"    {{ uint64_t r = {call}; cpu->eax = (uint32_t)r; cpu->edx = (uint32_t)(r >> 32); }}")
        elif rt in ("float", "double"):
            lines.append(f"    fpush(cpu, (rt_x87){call});")
        else:
            lines.append(f"    cpu->eax = (uint32_t){call};")
        lines += ["    cpu->esp += 4;", "    return 1;", "}", ""]
        out += lines
        names.append(name)
    hdr = ['/* generated by tools/recomp/gen_gl.py from the Khronos gl.xml — do not edit */',
           '#include "rt.h"', '#include "gl_host.h"', '']
    tab = ["const struct gl_thunk gl_thunks[] = {"]
    tab += [f'    {{ "{n}", glt_{n} }},' for n in names]
    tab += ["    { 0, 0 }", "};", f"const int gl_thunk_count = {len(names)};",
            f"/* hand-written in hostlibs.c: {', '.join(sorted(MANUAL))} */",
            f"/* not generated ({len(skipped)}): {', '.join(sorted(skipped))} */"]
    open(args.o, "w").write("\n".join(hdr + out + tab) + "\n")
    print(f"{len(names)} GL thunks, {len(skipped)} skipped, {len(MANUAL)} manual -> {args.o}")


if __name__ == "__main__":
    main()
