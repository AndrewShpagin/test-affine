#!/usr/bin/env python3
"""Optional Linux/Mesa test of the shipped GLSL, without a browser download.

Usage: python3 flowx_smooth_fill_egl_test.py build/restart-fixtures.json.js
Requires Node, libEGL.so.1 and a surfaceless OpenGL ES 3 implementation.
"""
import ctypes as C
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

P, I, U, F = C.c_void_p, C.c_int, C.c_uint, C.c_float
egl = C.CDLL('libEGL.so.1')


def egl_fn(name, result, args):
    fn = getattr(egl, name)
    fn.restype, fn.argtypes = result, args
    return fn


get_proc = egl_fn('eglGetProcAddress', P, [C.c_char_p])


def gl_fn(name, result, args):
    address = get_proc(name.encode())
    if not address:
        raise RuntimeError('Missing entry point: ' + name)
    return C.CFUNCTYPE(result, *args)(address)


def run():
    source = Path(sys.argv[1]).read_text()
    shaders = [re.search(r'const ' + name + r' = `([\s\S]*?)`;', source).group(1)
               for name in ('vs', 'smoothFillFs')]
    with tempfile.TemporaryDirectory() as temp:
        fixture = Path(temp) / 'cases.json'
        subprocess.run(['node', str(Path(__file__).with_name('flowx_smooth_fill_test.js')),
                        '--export', str(fixture)], check=True)
        cases = json.loads(fixture.read_text())

    display = gl_fn('eglGetPlatformDisplayEXT', P, [U, P, C.POINTER(I)])(0x31DD, None, None)
    major, minor = I(), I()
    if not egl_fn('eglInitialize', U, [P, P, P])(display, C.byref(major), C.byref(minor)):
        raise RuntimeError('Surfaceless EGL initialization failed')
    context = None
    current = egl_fn('eglMakeCurrent', U, [P, P, P, P])
    try:
        if not egl_fn('eglBindAPI', U, [U])(0x30A0):
            raise RuntimeError('OpenGL ES API unavailable')
        config, count = P(), I()
        attrs = (I * 13)(0x3033, 1, 0x3040, 0x40, 0x3024, 8,
                         0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038)
        choose = egl_fn('eglChooseConfig', U, [P, P, P, I, P])
        if not choose(display, attrs, C.byref(config), 1, C.byref(count)) or count.value != 1:
            raise RuntimeError('OpenGL ES 3 RGBA8 config unavailable')
        context = egl_fn('eglCreateContext', P, [P, P, P, P])(
            display, config, None, (I * 3)(0x3098, 3, 0x3038))
        if not context or not current(display, None, None, context):
            raise RuntimeError('OpenGL ES 3 context creation failed')

        create_shader = gl_fn('glCreateShader', U, [U])
        shader_source = gl_fn('glShaderSource', None, [U, I, P, P])
        compile_shader = gl_fn('glCompileShader', None, [U])
        get_shader = gl_fn('glGetShaderiv', None, [U, U, P])
        shader_log = gl_fn('glGetShaderInfoLog', None, [U, I, P, P])
        program = gl_fn('glCreateProgram', U, [])()
        for kind, text in zip((0x8B31, 0x8B30), shaders):
            shader = create_shader(kind)
            encoded = C.c_char_p(text.encode())
            shader_source(shader, 1, C.byref(encoded), None)
            compile_shader(shader)
            status = I()
            get_shader(shader, 0x8B81, C.byref(status))
            if not status.value:
                log = C.create_string_buffer(8192)
                shader_log(shader, len(log), None, log)
                raise AssertionError(log.value.decode())
            gl_fn('glAttachShader', None, [U, U])(program, shader)
            gl_fn('glDeleteShader', None, [U])(shader)
        gl_fn('glLinkProgram', None, [U])(program)
        status = I()
        gl_fn('glGetProgramiv', None, [U, U, P])(program, 0x8B82, C.byref(status))
        if not status.value:
            log = C.create_string_buffer(8192)
            gl_fn('glGetProgramInfoLog', None, [U, I, P, P])(program, len(log), None, log)
            raise AssertionError(log.value.decode())
        gl_fn('glUseProgram', None, [U])(program)
        gl_fn('glDisable', None, [U])(0x0BD0)  # No dithering in byte comparisons.
        vao, framebuffer = U(), U()
        gl_fn('glGenVertexArrays', None, [I, P])(1, C.byref(vao))
        gl_fn('glBindVertexArray', None, [U])(vao)
        gl_fn('glGenFramebuffers', None, [I, P])(1, C.byref(framebuffer))
        gl_fn('glBindFramebuffer', None, [U, U])(0x8D40, framebuffer)
        textures = (U * 3)()
        gl_fn('glGenTextures', None, [I, P])(3, textures)
        active = gl_fn('glActiveTexture', None, [U])
        bind = gl_fn('glBindTexture', None, [U, U])
        param = gl_fn('glTexParameteri', None, [U, U, I])
        upload = gl_fn('glTexImage2D', None, [U, I, I, I, I, I, U, U, P])
        for unit, texture in enumerate(textures):
            active(0x84C0 + unit)
            bind(0x0DE1, texture)
            for key, value in ((0x2801, 0x2601), (0x2800, 0x2601),
                               (0x2802, 0x812F), (0x2803, 0x812F)):
                param(0x0DE1, key, value)
        location = gl_fn('glGetUniformLocation', I, [U, C.c_char_p])
        uniform = gl_fn('glUniform1i', None, [I, I])
        uniform(location(program, b'uNearest'), 0)
        uniform(location(program, b'uFillParams'), 1)
        worst = 0
        for case in cases:
            width, height = case['width'], case['height']
            for unit, internal, fmt, data in ((0, 0x8058, 0x1908, case['pixels']),
                                             (1, 0x822B, 0x8227, case['params']),
                                             (2, 0x8058, 0x1908, None)):
                active(0x84C0 + unit)
                buffer = (C.c_ubyte * len(data))(*data) if data is not None else None
                upload(0x0DE1, 0, internal, width, height, 0, fmt, 0x1401, buffer)
            gl_fn('glFramebufferTexture2D', None, [U, U, U, U, I])(
                0x8D40, 0x8CE0, 0x0DE1, textures[2], 0)
            if gl_fn('glCheckFramebufferStatus', U, [U])(0x8D40) != 0x8CD5:
                raise AssertionError('Incomplete smoothing framebuffer')
            gl_fn('glViewport', None, [I, I, I, I])(0, 0, width, height)
            gl_fn('glUniform2f', None, [I, F, F])(location(program, b'uSize'), width, height)
            gl_fn('glDrawArrays', None, [U, I, I])(4, 0, 3)
            output = (C.c_ubyte * (width * height * 4))()
            gl_fn('glReadPixels', None, [I, I, I, I, U, U, P])(
                0, 0, width, height, 0x1908, 0x1401, output)
            error = gl_fn('glGetError', U, [])()
            if error:
                raise AssertionError('GL error: ' + hex(error))
            for i, (actual, expected) in enumerate(zip(output, case['expected'])):
                delta = abs(actual - expected)
                worst = max(worst, delta)
                # Allow byte rounding in bilinear texture filtering only.
                tolerance = 2 if case['params'][2 * (i // 4) + 1] and i % 4 != 3 else 0
                if delta > tolerance:
                    raise AssertionError(f"{case['name']} byte {i}: GPU {actual}, CPU {expected}")
        renderer = gl_fn('glGetString', C.c_char_p, [U])(0x1F01).decode()
        print(f'PASS: shipped GLSL, {len(cases)} cases, max byte error {worst}; {renderer}')
    finally:
        current(display, None, None, None)
        if context:
            egl_fn('eglDestroyContext', U, [P, P])(display, context)
        egl_fn('eglTerminate', U, [P])(display)


if __name__ == '__main__':
    run()
