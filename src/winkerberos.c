/*
 * Copyright 2016 MongoDB, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "kerberos_sspi.h"

#include <Shlwapi.h>

#if PY_MAJOR_VERSION >= 3
#define PyInt_FromLong PyLong_FromLong
#define PyString_FromString PyUnicode_FromString
#endif

#if PY_VERSION_HEX >= 0x03020000
#define PyCObject_Check PyCapsule_CheckExact
#define PyCObject_FromVoidPtr(cobj, destr) PyCapsule_New(cobj, NULL, destr)
#define PyCObject_AsVoidPtr(self) PyCapsule_GetPointer(self, NULL)
#endif

#if PY_VERSION_HEX < 0x03030000
#define PyUnicode_GET_LENGTH PyUnicode_GET_SIZE
#endif

PyDoc_STRVAR(winkerberos_documentation,
"A native Kerberos SSPI client implementation.\n"
"\n"
"This module mimics the client API of pykerberos to implement\n"
"Kerberos SSPI authentication on Microsoft Windows.");

PyObject* KrbError;
/* Note - also defined extern in kerberos_sspi.c */
PyObject* GSSError;

static BOOL
_string_too_long(const SEC_CHAR* key, SIZE_T len) {
    if (len > ULONG_MAX) {
        PyErr_Format(PyExc_ValueError, "%s too large", key);
        return TRUE;
    }
    return FALSE;
}

static BOOL
_py_buffer_to_wchar(PyObject* obj, WCHAR** out, Py_ssize_t* outlen) {
    Py_buffer view;
    WCHAR* outbuf;
    INT result_len;
    BOOL result = FALSE;
    if (PyObject_GetBuffer(obj, &view, PyBUF_SIMPLE) == -1) {
        return FALSE;
    }
    if (!PyBuffer_IsContiguous(&view, 'C')) {
        PyErr_SetString(PyExc_ValueError,
                        "must be a contiguous buffer");
        goto done;
    }
    if (!view.buf || view.len < 0) {
        PyErr_SetString(PyExc_ValueError, "invalid buffer");
        goto done;
    }
    if (view.itemsize != 1) {
        PyErr_SetString(PyExc_ValueError,
                        "buffer data must be ascii or utf8");
        goto done;
    }
    if (view.len > INT_MAX) {
        /* MultiByteToWideChar expects length as a signed int. */
        PyErr_SetString(PyExc_ValueError, "buffer too large");
        goto done;
    }
    outbuf = (WCHAR*)malloc(sizeof(WCHAR) * (view.len + 1));
    if (!outbuf) {
        PyErr_SetNone(PyExc_MemoryError);
        goto done;
    }
    result_len = MultiByteToWideChar(
        CP_UTF8, 0, (CHAR*)view.buf, (INT)view.len, outbuf, (INT)view.len);
    if (!result_len) {
        set_gsserror(GetLastError(), "MultiByteToWideChar failed");
        free(outbuf);
        goto done;
    }
    outbuf[result_len] = L'\0';
    *outlen = (Py_ssize_t)result_len;
    *out = outbuf;
    result = TRUE;
done:
    PyBuffer_Release(&view);
    return result;
}

static BOOL
_py_unicode_to_wchar(PyObject* obj, WCHAR** out, Py_ssize_t* outlen) {
    Py_ssize_t res;
    Py_ssize_t len = PyUnicode_GET_LENGTH(obj);
    WCHAR* buf = (WCHAR*)malloc(sizeof(WCHAR) * (len + 1));
    if (!buf) {
        PyErr_SetNone(PyExc_MemoryError);
        return FALSE;
    }
#if PY_VERSION_HEX < 0x03020000
    res = PyUnicode_AsWideChar((PyUnicodeObject*)obj, buf, len);
#else
    res = PyUnicode_AsWideChar(obj, buf, len);
#endif
    if (res == -1) {
        goto fail;
    }
    buf[len] = L'\0';
    if (wcslen(buf) != (size_t)len) {
        PyErr_SetString(PyExc_ValueError, "embedded null character");
        goto fail;
    }
    *out = buf;
    *outlen = len;
    return TRUE;

fail:
    free(buf);
    return FALSE;
}

static BOOL
BufferObject_AsWCHAR(PyObject* arg, WCHAR** out, Py_ssize_t* outlen) {
    if (arg == Py_None) {
        *out = NULL;
        *outlen = 0;
        return TRUE;
    } else if (PyUnicode_Check(arg)){
        return _py_unicode_to_wchar(arg, out, outlen);
    } else {
        return _py_buffer_to_wchar(arg, out, outlen);
    }
}

static BOOL
StringObject_AsWCHAR(PyObject* arg,
                     INT argnum,
                     BOOL allow_none,
                     WCHAR** out,
                     Py_ssize_t* outlen) {
    if (arg == Py_None && allow_none) {
        *out = NULL;
        *outlen = 0;
        return TRUE;
#if PY_MAJOR_VERSION < 3
    } else if (PyString_Check(arg)) {
        BOOL result;
        PyObject* localobj = PyUnicode_FromEncodedObject(arg, NULL, "strict");
        if (!localobj) {
            return FALSE;
        }
        result = _py_unicode_to_wchar(localobj, out, outlen);
        Py_DECREF(localobj);
        return result;
#endif
    } else if (PyUnicode_Check(arg)){
        return _py_unicode_to_wchar(arg, out, outlen);
    } else {
        PyErr_Format(
           PyExc_TypeError,
#if PY_MAJOR_VERSION < 3
           "argument %d must be string%s, not %s",
#else
           "argument %d must be str%s, not %s",
#endif
           argnum,
           allow_none ? " or None" : "",
           (arg == Py_None) ? "None" : arg->ob_type->tp_name);
        return FALSE;
    }
}

static VOID
#if PY_MAJOR_VERSION >=3
destroy_sspi_client(PyObject* obj) {
    sspi_client_state* state = PyCapsule_GetPointer(obj, NULL);
#else
destroy_sspi_client(VOID* obj) {
    sspi_client_state* state = (sspi_client_state*)obj;
#endif
    if (state) {
        destroy_sspi_client_state(state);
        free(state);
    }
}

PyDoc_STRVAR(sspi_client_init_doc,
"authGSSClientInit(service, principal=None, gssflags="
"GSS_C_MUTUAL_FLAG|GSS_C_SEQUENCE_FLAG, user=None, domain=None,"
" password=None)\n"
"\n"
"Initializes a context for Kerberos SSPI client side authentication with\n"
"the given service principal.\n"
"\n"
"The following flags are available (with SSPI mapping)::\n"
"\n"
"  GSS_C_DELEG_FLAG    (ISC_REQ_DELEG)\n"
"  GSS_C_MUTUAL_FLAG   (ISC_REQ_MUTUAL_AUTH)\n"
"  GSS_C_REPLAY_FLAG   (ISC_REQ_REPLAY_DETECT)\n"
"  GSS_C_SEQUENCE_FLAG (ISC_REQ_SEQUENCE_DETECT)\n"
"  GSS_C_CONF_FLAG     (ISC_REQ_CONFIDENTIALITY)\n"
"  GSS_C_INTEG_FLAG    (ISC_REQ_INTEGRITY)\n"
"\n"
"The following flags are *not* available as they have no mapping in SSPI::\n"
"\n"
"  GSS_C_ANON_FLAG\n"
"  GSS_C_PROT_READY_FLAG\n"
"  GSS_C_TRANS_FLAG\n"
"\n"
":Parameters:\n"
"  - `service`: A string containing the service principal in RFC-2078 format\n"
"    (``service@hostname``) or SPN format (``service/hostname`` or\n"
"    ``service/hostname@REALM``).\n"
"  - `principal`: An optional string containing the user principal name in\n"
"    the format ``user@realm``. Can be unicode (str in python 3.x) or any 8 \n"
"    bit string type that implements the buffer interface. A password can \n"
"    be provided using the format ``user@realm:password``. The principal \n"
"    and password can be percent encoded if either might include the ``:`` \n"
"    character::\n"
"\n"
"      try:\n"
"          # Python 3.x\n"
"          from urllib.parse import quote\n"
"      except ImportError:\n"
"          # Python 2.x\n"
"          from urllib import quote\n"
"      principal = '%s:%s' % (\n"
"          quote(user_principal), quote(password))\n"
"\n"
"    If the `user` parameter is provided `principal` is ignored.\n"
"  - `gssflags`: An optional integer used to set GSS flags. Defaults to\n"
"    GSS_C_MUTUAL_FLAG|GSS_C_SEQUENCE_FLAG.\n"
"  - `user` (DEPRECATED): An optional string that contains the name of the \n"
"    user whose credentials should be used for authentication.\n"
"  - `domain` (DEPRECATED): An optional string that contains the domain or \n"
"    workgroup name for `user`.\n"
"  - `password` (DEPRECATED): An optional string that contains the password \n"
"    for `user` in `domain`. Can be unicode (str in python 3.x) or any 8 \n"
"    bit string type that implements the buffer interface.\n"
"\n"
":Returns: A tuple of (result, context) where result is\n"
"          :data:`AUTH_GSS_COMPLETE` and context is an opaque value passed\n"
"          in subsequent function calls.");

static PyObject*
sspi_client_init(PyObject* self, PyObject* args, PyObject* kw) {
    sspi_client_state* state;
    PyObject* pyctx = NULL;
    PyObject* serviceobj;
    PyObject* principalobj = Py_None;
    LONG flags = ISC_REQ_MUTUAL_AUTH | ISC_REQ_SEQUENCE_DETECT;
    PyObject* userobj = Py_None;
    PyObject* domainobj = Py_None;
    PyObject* passwordobj = Py_None;
    WCHAR *service = NULL, *principal = NULL;
    WCHAR *user = NULL, *domain = NULL, *password = NULL;
    Py_ssize_t slen, len, ulen, dlen, plen = 0;
    PyObject* resultobj = NULL;
    INT result = 0;
    static SEC_CHAR* keywords[] = {
        "service", "principal", "gssflags", "user", "domain", "password", NULL};

    if (!PyArg_ParseTupleAndKeywords(args,
                                     kw,
                                     "O|OlOOO",
                                     keywords,
                                     &serviceobj,
                                     &principalobj,
                                     &flags,
                                     &userobj,
                                     &domainobj,
                                     &passwordobj)) {
        return NULL;
    }
    if (flags < 0) {
        PyErr_SetString(PyExc_ValueError, "gss_flags must be >= 0");
        return NULL;
    }

    if (!StringObject_AsWCHAR(serviceobj, 1, FALSE, &service, &slen) ||
        !BufferObject_AsWCHAR(principalobj, &principal, &len) ||
        !StringObject_AsWCHAR(userobj, 4, TRUE, &user, &ulen) ||
        !StringObject_AsWCHAR(domainobj, 5, TRUE, &domain, &dlen) ||
        !BufferObject_AsWCHAR(passwordobj, &password, &plen) ||
        _string_too_long("user", (SIZE_T)ulen) ||
        _string_too_long("domain", (SIZE_T)dlen) ||
        _string_too_long("password", (SIZE_T)plen)) {
        goto done;
    }

    /* Prefer (user, domain, password) for backward compatibility. */
    if (!user && principal) {
        WCHAR* userDomain = NULL;
        HRESULT res;
        /* Use (user, domain, password) or principal, not a mix of both. */
        free(domain);
        domain = NULL;
        if (password) {
            SecureZeroMemory(password, sizeof(WCHAR) * plen);
            free(password);
            password = NULL;
        }
        /* Support password as part of the principal parameter. */
        if (wcschr(principal, L':')) {
            WCHAR* next;
            userDomain = _wcsdup(wcstok_s(principal, L":", &next));
            password = _wcsdup(wcstok_s(NULL, L":", &next));
        } else {
            userDomain = _wcsdup(principal);
        }
        /* Support user principal or password including the : character. */
        res = UrlUnescapeW(userDomain, NULL, NULL, URL_UNESCAPE_INPLACE);
        if (res != S_OK) {
            free(userDomain);
            set_gsserror(res, "UrlUnescapeW");
            goto done;
        }
        if (password) {
            res = UrlUnescapeW(password, NULL, NULL, URL_UNESCAPE_INPLACE);
            if (res != S_OK) {
                free(userDomain);
                set_gsserror(res, "UrlUnescapeW");
                goto done;
            }
            plen = wcslen(password);
        }
        user = userDomain;
        ulen = wcslen(userDomain);
    }

    state = (sspi_client_state*)malloc(sizeof(sspi_client_state));
    if (state == NULL) {
        PyErr_SetNone(PyExc_MemoryError);
        goto done;
    }

    pyctx = PyCObject_FromVoidPtr(state, &destroy_sspi_client);
    if (pyctx == NULL) {
        free(state);
        goto done;
    }

    result = auth_sspi_client_init(
        service, (ULONG)flags,
        user, (ULONG)ulen, domain, (ULONG)dlen, password, (ULONG)plen, state);
    if (result == AUTH_GSS_ERROR) {
        Py_DECREF(pyctx);
        goto done;
    }

    resultobj =  Py_BuildValue("(iN)", result, pyctx);

done:
    free(service);
    /* The principal parameter can include a password. */
    if (principal) {
        SecureZeroMemory(principal, sizeof(WCHAR) * len);
        free(principal);
    }
    free(user);
    free(domain);
    if (password) {
        SecureZeroMemory(password, sizeof(WCHAR) * plen);
        free(password);
    }
    return resultobj;
}

PyDoc_STRVAR(sspi_client_clean_doc,
"authGSSClientClean(context)\n"
"\n"
"Destroys the context. This function is provided for API compatibility with\n"
"pykerberos but does nothing. The context object destroys itself when it\n"
"is reclaimed.\n"
"\n"
":Parameters:\n"
"  - `context`: The context object returned by :func:`authGSSClientInit`.\n"
"\n"
":Returns: :data:`AUTH_GSS_COMPLETE`");

static PyObject*
sspi_client_clean(PyObject* self, PyObject* args) {
    /* Do nothing. For compatibility with pykerberos only. */
    return Py_BuildValue("i", AUTH_GSS_COMPLETE);
}

PyDoc_STRVAR(sspi_client_step_doc,
"authGSSClientStep(context, challenge)\n"
"\n"
"Executes a single Kerberos SSPI client step using the supplied server "
"challenge.\n"
"\n"
":Parameters:\n"
"  - `context`: The context object returned by :func:`authGSSClientInit`.\n"
"  - `challenge`: A string containing the base64 encoded server challenge.\n"
"    Ignored for the first step (pass the empty string).\n"
"\n"
":Returns: :data:`AUTH_GSS_CONTINUE` or :data:`AUTH_GSS_COMPLETE`");

static PyObject*
sspi_client_step(PyObject* self, PyObject* args) {
    sspi_client_state* state;
    PyObject* pyctx;
    SEC_CHAR* challenge = NULL;
    INT result = 0;

    if (!PyArg_ParseTuple(args, "Os", &pyctx, &challenge)) {
        return NULL;
    }

    if (_string_too_long("challenge", strlen(challenge))) {
        return NULL;
    }

    if (!PyCObject_Check(pyctx)) {
        PyErr_SetString(PyExc_TypeError, "Expected a context object");
        return NULL;
    }

    state = (sspi_client_state*)PyCObject_AsVoidPtr(pyctx);
    if (state == NULL) {
        return NULL;
    }

    result = auth_sspi_client_step(state, challenge);
    if (result == AUTH_GSS_ERROR) {
        return NULL;
    }

    return Py_BuildValue("i", result);
}

PyDoc_STRVAR(sspi_client_response_doc,
"authGSSClientResponse(context)\n"
"\n"
"Get the response to the last successful client operation.\n"
"\n"
":Parameters:\n"
"  - `context`: The context object returned by :func:`authGSSClientInit`.\n"
"\n"
":Returns: A base64 encoded string to return to the server.");

static PyObject*
sspi_client_response(PyObject* self, PyObject* args) {
    sspi_client_state* state;
    PyObject* pyctx;

    if (!PyArg_ParseTuple(args, "O", &pyctx)) {
        return NULL;
    }

    if (!PyCObject_Check(pyctx)) {
        PyErr_SetString(PyExc_TypeError, "Expected a context object");
        return NULL;
    }

    state = (sspi_client_state*)PyCObject_AsVoidPtr(pyctx);
    if (state == NULL) {
        return NULL;
    }

    return Py_BuildValue("s", state->response);
}

PyDoc_STRVAR(sspi_client_username_doc,
"authGSSClientUsername(context)\n"
"\n"
"Get the user name of the authenticated principal. Will only succeed after\n"
"authentication is complete.\n"
"\n"
":Parameters:\n"
"  - `context`: The context object returned by :func:`authGSSClientInit`.\n"
"\n"
":Returns: A string containing the username.");

static PyObject*
sspi_client_username(PyObject* self, PyObject* args) {
    sspi_client_state* state;
    PyObject* pyctx;

    if (!PyArg_ParseTuple(args, "O", &pyctx)) {
        return NULL;
    }

    if (!PyCObject_Check(pyctx)) {
        PyErr_SetString(PyExc_TypeError, "Expected a context object");
        return NULL;
    }

    state = (sspi_client_state*)PyCObject_AsVoidPtr(pyctx);
    if (state == NULL) {
        return NULL;
    }

    return Py_BuildValue("s", state->username);
}

PyDoc_STRVAR(sspi_client_unwrap_doc,
"authGSSClientUnwrap(context, challenge)\n"
"\n"
"Execute the client side DecryptMessage (GSSAPI Unwrap) operation.\n"
"\n"
":Parameters:\n"
"  - `context`: The context object returned by :func:`authGSSClientInit`.\n"
"  - `challenge`: A string containing the base64 encoded server\n"
"    challenge.\n"
"\n"
":Returns: :data:`AUTH_GSS_COMPLETE`");

static PyObject*
sspi_client_unwrap(PyObject* self, PyObject* args) {
    sspi_client_state* state;
    PyObject* pyctx;
    SEC_CHAR* challenge;
    INT result;

    if (!PyArg_ParseTuple(args, "Os", &pyctx, &challenge)) {
        return NULL;
    }

    if (_string_too_long("challenge", strlen(challenge))) {
        return NULL;
    }

    if (!PyCObject_Check(pyctx)) {
        PyErr_SetString(PyExc_TypeError, "Expected a context object");
        return NULL;
    }

    state = (sspi_client_state*)PyCObject_AsVoidPtr(pyctx);
    if (state == NULL) {
        return NULL;
    }

    result = auth_sspi_client_unwrap(state, challenge);
    if (result == AUTH_GSS_ERROR) {
        return NULL;
    }

    return Py_BuildValue("i", result);
}

PyDoc_STRVAR(sspi_client_wrap_doc,
"authGSSClientWrap(context, data, user=None)\n"
"\n"
"Execute the client side EncryptMessage (GSSAPI Wrap) operation.\n"
"\n"
":Parameters:\n"
"  - `context`: The context object returned by :func:`authGSSClientInit`.\n"
"  - `data`: If `user` is not None, this should be the result of calling\n"
"    :func:`authGSSClientResponse` after :func:`authGSSClientUnwrap`.\n"
"    If `user` is None, this should be a base64 encoded authorization\n"
"    message as specified in Section 3.1 of RFC-4752.\n"
"  - `user`: An optional string containing the user principal to authorize.\n"
"\n"
":Returns: :data:`AUTH_GSS_COMPLETE`");

static PyObject*
sspi_client_wrap(PyObject* self, PyObject* args) {
    sspi_client_state* state;
    PyObject* pyctx;
    SEC_CHAR* data;
    SEC_CHAR* user = NULL;
    SIZE_T ulen = 0;
    INT result;

    if (!PyArg_ParseTuple(args, "Os|z", &pyctx, &data, &user)) {
        return NULL;
    }
    if (user) {
        ulen = strlen(user);
    }

    if (_string_too_long("data", strlen(data)) ||
        /* Length of user + 4 bytes for security options. */
        _string_too_long("user", ulen + 4)) {
        return NULL;
    }

    if (!PyCObject_Check(pyctx)) {
        PyErr_SetString(PyExc_TypeError, "Expected a context object");
        return NULL;
    }

    state = (sspi_client_state*)PyCObject_AsVoidPtr(pyctx);
    if (state == NULL) {
        return NULL;
    }

    result = auth_sspi_client_wrap(state, data, user, (ULONG)ulen);
    if (result == AUTH_GSS_ERROR) {
        return NULL;
    }

    return Py_BuildValue("i", result);
}

static PyMethodDef WinKerberosClientMethods[] = {
    {"authGSSClientInit", (PyCFunction)sspi_client_init,
     METH_VARARGS | METH_KEYWORDS, sspi_client_init_doc},
    {"authGSSClientClean", sspi_client_clean,
     METH_VARARGS, sspi_client_clean_doc},
    {"authGSSClientStep", sspi_client_step,
     METH_VARARGS, sspi_client_step_doc},
    {"authGSSClientResponse", sspi_client_response,
     METH_VARARGS, sspi_client_response_doc},
    {"authGSSClientUsername", sspi_client_username,
     METH_VARARGS, sspi_client_username_doc},
    {"authGSSClientUnwrap", sspi_client_unwrap,
     METH_VARARGS, sspi_client_unwrap_doc},
    {"authGSSClientWrap", sspi_client_wrap,
     METH_VARARGS, sspi_client_wrap_doc},
    {NULL, NULL, 0, NULL}
};

#if PY_MAJOR_VERSION >= 3
#define INITERROR return NULL

static struct PyModuleDef moduledef = {
    PyModuleDef_HEAD_INIT,
    "winkerberos",
    winkerberos_documentation,
    -1,
    WinKerberosClientMethods,
    NULL,
    NULL,
    NULL,
    NULL,
};

PyMODINIT_FUNC
PyInit_winkerberos(VOID)
#else
#define INITERROR return
PyMODINIT_FUNC
initwinkerberos(VOID)
#endif
{
#if PY_MAJOR_VERSION >= 3
    PyObject* module = PyModule_Create(&moduledef);
#else
    PyObject* module = Py_InitModule3(
        "winkerberos",
        WinKerberosClientMethods,
        winkerberos_documentation);
#endif
    if (module == NULL) {
        INITERROR;
    }

    KrbError = PyErr_NewException(
        "winkerberos.KrbError", NULL, NULL);
    if (KrbError == NULL) {
        Py_DECREF(module);
        INITERROR;
    }
    Py_INCREF(KrbError);
    
    GSSError = PyErr_NewException(
        "winkerberos.GSSError", KrbError, NULL);
    if (GSSError == NULL) {
        Py_DECREF(KrbError);
        Py_DECREF(module);
        INITERROR;
    }
    Py_INCREF(GSSError);

    if (PyModule_AddObject(module,
                           "KrbError",
                           KrbError) ||
        PyModule_AddObject(module,
                           "GSSError",
                           GSSError) ||
        PyModule_AddObject(module,
                           "AUTH_GSS_COMPLETE",
                           PyInt_FromLong(AUTH_GSS_COMPLETE)) ||
        PyModule_AddObject(module,
                           "AUTH_GSS_CONTINUE",
                           PyInt_FromLong(AUTH_GSS_CONTINUE)) ||
        PyModule_AddObject(module,
                           "GSS_C_DELEG_FLAG",
                           PyInt_FromLong(ISC_REQ_DELEGATE)) ||
        PyModule_AddObject(module,
                           "GSS_C_MUTUAL_FLAG",
                           PyInt_FromLong(ISC_REQ_MUTUAL_AUTH)) ||
        PyModule_AddObject(module,
                           "GSS_C_REPLAY_FLAG",
                           PyInt_FromLong(ISC_REQ_REPLAY_DETECT)) ||
        PyModule_AddObject(module,
                           "GSS_C_SEQUENCE_FLAG",
                           PyInt_FromLong(ISC_REQ_SEQUENCE_DETECT)) ||
        PyModule_AddObject(module,
                           "GSS_C_CONF_FLAG",
                           PyInt_FromLong(ISC_REQ_CONFIDENTIALITY)) ||
        PyModule_AddObject(module,
                           "GSS_C_INTEG_FLAG",
                           PyInt_FromLong(ISC_REQ_INTEGRITY)) ||
        PyModule_AddObject(module,
                           "__version__",
                           PyString_FromString("0.5.0.dev0"))) {
        Py_DECREF(GSSError);
        Py_DECREF(KrbError);
        Py_DECREF(module);
        INITERROR;
    }

#if PY_MAJOR_VERSION >= 3
    return module;
#endif
}
