/*
 * python-ubus - python bindings for ubus
 *
 * Copyright (C) 2017-2018 Stepan Henek <stepan.henek@nic.cz>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */


#include <Python.h>
#include <dlfcn.h>
#include <libubox/blobmsg_json.h>
#include <libubus.h>
#include <stdio.h>

#ifndef UBUS_UNIX_SOCKET
#define UBUS_UNIX_SOCKET "/var/run/ubus/ubus.sock"
#endif

#define DEFAULT_SOCKET UBUS_UNIX_SOCKET
#define RESPONSE_HANDLER_OBJECT_NAME "ubus.__ResponseHandler"

#define MSG_ALLOCATION_FAILS "Failed to allocate memory!"
#define MSG_LISTEN_TUPLE_EXPECTED "Expected (event, callback) tuple"
#define MSG_ADD_SIGNATURE_INVALID \
"Incorrect method arguments!\n" \
"Expected:\n" \
"	(<obj_name>, { " \
	"<method_name>: {'signature': <method_signature>, 'method': <callable>}" \
", ...})"
#define MSG_JSON_TO_UBUS_FAILED "Failed to create json for ubus."
#define MSG_JSON_FROM_UBUS_FAILED "Failed to create json from ubus."
#define MSG_NOT_CONNECTED "You are not connected to ubus."
#define MSG_ALREADY_CONNECTED "You are already connected to ubus."

struct module_state {
    PyObject *error;
};

#if PY_MAJOR_VERSION >= 3
#define GETSTATE(m) ((struct module_state*)PyModule_GetState(m))
#define PyStr_Check PyUnicode_Check
#define PyInt_Check PyLong_Check

#else
#define PyStr_Check(x) (PyString_Check(x) || PyUnicode_Check(x))
#define PyUnicode_AsUTF8 PyString_AsString
#define GETSTATE(m) (&_state)
static struct module_state _state;
#endif

typedef struct {
	struct ubus_object object;
	PyObject *methods;
} ubus_Object;

typedef struct {
	struct ubus_event_handler handler;
	PyObject *callback;
}ubus_Listener ;


PyObject *prepare_bool(bool yes)
{
	if (yes) {
		Py_INCREF(Py_True);
		return Py_True;
	} else {
		Py_INCREF(Py_False);
		return Py_False;
	}
}

/* ubus module objects */
static PyMethodDef ubus_methods[];
PyObject *python_alloc_list = NULL;
char *socket_path = NULL;
ubus_Listener **listeners = NULL;
size_t listerners_size = 0;
ubus_Object **objects = NULL;
size_t objects_size = 0;
struct blob_buf python_buf;
struct ubus_context *ctx = NULL;

#define CONNECTED (ctx != NULL)


/* json module handlers */
PyObject *json_module = NULL;

enum json_function {
	LOADS,
	DUMPS,
};

const char *json_function_names[2] = {
	[LOADS] = "loads",
	[DUMPS] = "dumps",
};


PyObject *perform_json_function(enum json_function json_function, PyObject *input)
{
	PyObject *function = PyObject_GetAttrString(json_module, json_function_names[json_function]);
	if (!function) {
		return NULL;
	}
	PyObject *arglist = Py_BuildValue("(O)", input);
	if (!arglist) {
		Py_DECREF(function);
		return NULL;
	}
	PyObject *data_object = PyObject_CallObject(function, arglist);
	Py_DECREF(function);
	Py_DECREF(arglist);

	return data_object;  // New reference - should be decreased by the caller
}

/* ResponseHandler */

typedef struct {
	PyObject_HEAD
	struct ubus_context *ctx;
	struct ubus_request_data *req;
	struct blob_buf buf;
} ubus_ResponseHandler;

static void ubus_ResponseHandler_dealloc(ubus_ResponseHandler* self)
{
	blob_buf_free(&self->buf);
	Py_TYPE(self)->tp_free((PyObject*)self);
}

PyDoc_STRVAR(
	ResponseHandler_reply_doc,
	"reply(data)\n"
	"\n"
	":param data: JSON to be send as a response to a ubus call.\n"
	":type data: dict\n"
);

static PyObject *ubus_ResponseHandler_reply(ubus_ResponseHandler *self, PyObject *args, PyObject *kwargs)
{
	if (!CONNECTED) {
		PyErr_Format(PyExc_RuntimeError, MSG_NOT_CONNECTED);
		return NULL;
	}

	PyObject *data = NULL;
	static char *kwlist[] = {"data",  NULL};
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "O", kwlist, &data)) {
		return NULL;
	}

	// Call python function json.dumps
	PyObject *json_str = perform_json_function(DUMPS, data);
	if (!json_str) {
		return NULL;
	}

	// put json string into buffer
	blob_buf_init(&self->buf, 0);
	bool res = blobmsg_add_json_from_string(&self->buf, PyUnicode_AsUTF8(json_str));
	Py_DECREF(json_str);
	if (!res) {
		PyErr_Format(PyExc_TypeError, MSG_JSON_TO_UBUS_FAILED);
		return NULL;
	}

	// handler is not linked to a call response
	if (!self->req || !self->ctx) {
		PyErr_Format(PyExc_RuntimeError, "Handler is not linked to a call response.");
		return NULL;
	}

	int retval = ubus_send_reply(self->ctx, self->req, self->buf.head);
	return prepare_bool(!retval);
}

PyDoc_STRVAR(
	ResponseHandler_doc,
	"__ResponseHandler\n"
	"\n"
	"Object which is used to handle responses to ubus calls.\n"
);

static PyMethodDef ubus_ResponseHandler_methods[] = {
	{"reply", (PyCFunction)ubus_ResponseHandler_reply, METH_VARARGS|METH_KEYWORDS, ResponseHandler_reply_doc},
	{NULL},
};

static int ubus_ResponseHandler_init(ubus_ResponseHandler *self, PyObject *args, PyObject *kwargs)
{
	static char *kwlist[] = {NULL};
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "", kwlist)){
		return -1;
	}
	memset(&self->buf, 0, sizeof(self->buf));
	self->ctx = NULL;
	self->req = NULL;
	return 0;
}

static PyObject *ubus_ResponseHandler_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
	ubus_ResponseHandler *self = (ubus_ResponseHandler *)type->tp_alloc(type, 0);
	return (PyObject *)self;
}

static PyTypeObject ubus_ResponseHandlerType = {
	PyVarObject_HEAD_INIT(NULL, 0)
	RESPONSE_HANDLER_OBJECT_NAME,				/* tp_name */
	sizeof(ubus_ResponseHandler),				/* tp_basicsize */
	0,											/* tp_itemsize */
	(destructor)ubus_ResponseHandler_dealloc,	/* tp_dealloc */
	0,											/* tp_print */
	0,											/* tp_getattr */
	0,											/* tp_setattr */
	0,											/* tp_compare */
	0,											/* tp_repr */
	0,											/* tp_as_number */
	0,											/* tp_as_sequence */
	0,											/* tp_as_mapping */
	0,											/* tp_hash */
	0,											/* tp_call */
	0,											/* tp_str */
	0,											/* tp_getattro */
	0,											/* tp_setattro */
	0,											/* tp_as_buffer */
	Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,	/* tp_flags */
	ResponseHandler_doc,						/* tp_doc */
	0,											/* tp_traverse */
	0,											/* tp_clear */
	0,											/* tp_richcompare */
	0,											/* tp_weaklistoffset */
	0,											/* tp_iter */
	0,											/* tp_iternext */
	ubus_ResponseHandler_methods,				/* tp_methods */
	0,											/* tp_members */
	0,											/* tp_getset */
	0,											/* tp_base */
	0,											/* tp_dict */
	0,											/* tp_descr_get */
	0,											/* tp_descr_set */
	0,											/* tp_dictoffset */
	(initproc)ubus_ResponseHandler_init,		/* tp_init */
	0,											/* tp_alloc */
	ubus_ResponseHandler_new,					/* tp_new */
};

typedef struct {
	PyObject_HEAD
	PyObject *socket_path;
	ubus_Listener **listeners;
	size_t listerners_size;
	ubus_Object **objects;
	size_t objects_size;
	PyObject *alloc_list;  // Used for easy deallocation
	struct blob_buf buf;
	struct ubus_context *ctx;
} ubus_Connection;

void free_ubus_object(ubus_Object *obj)
{
	if (obj->object.methods) {
		for (int i = 0; i < obj->object.n_methods; i++) {
			if (&obj->object.methods[i] && obj->object.methods[i].policy) {
				free((struct blobmsg_policy *)obj->object.methods[i].policy);
			}
		}
		free((struct ubus_method *)obj->object.methods);
	}

	if (obj->object.type) {
		free(obj->object.type);
	}
	free(obj);
}


#if PY_MAJOR_VERSION >= 3
static struct PyModuleDef moduledef = {
        PyModuleDef_HEAD_INIT,
        "ubus",
        NULL,
        sizeof(struct module_state),
        ubus_methods,
        NULL,
        NULL,
        NULL,
        NULL
};
#endif

PyObject *ubus_python_module_init(void)
{
#if PY_MAJOR_VERSION >= 3
	PyObject *module = PyModule_Create(&moduledef);
#else
	PyObject *module = Py_InitModule3("ubus", ubus_methods, "Ubus bindings");
#endif
	return module;
}

PyDoc_STRVAR(
	disconnect_doc,
	"disconnect(deregister=True)\n"
	"\n"
	":param deregister: Deregisters object and handlers from ubus as well.\n"
	":type deregister: bool\n"
	"Disconnects from ubus and disposes all connection structures.\n"
);

void dispose_connection(bool deregister)
{
	if (ctx != NULL) {
		if (deregister) {
			// remove objects
			for (int i = 0; i < objects_size; i++) {
				ubus_remove_object(ctx, &objects[i]->object);
			}

			// remove listeners
			for (int i = 0; i < listerners_size; i++) {
				ubus_unregister_event_handler(ctx, &listeners[i]->handler);
			}
		}

		ubus_free(ctx);
		ctx = NULL;
	}
	uloop_done();
	blob_buf_free(&python_buf);
	if (python_alloc_list) {
		Py_DECREF(python_alloc_list);
		python_alloc_list = NULL;
	}
	// clear event listeners
	if (listeners) {
		for (int i = 0; i < listerners_size; i++) {
			free(listeners[i]);
		}
		free(listeners);
		listerners_size = 0;
		listeners = NULL;
	}
	// clear objects
	if (objects) {
		for (int i = 0; i < objects_size; i++) {
			free_ubus_object(objects[i]);
		}
		free(objects);
		objects_size = 0;
		objects = NULL;
	}

	if (socket_path) {
		free(socket_path);
		socket_path = NULL;
	}
}

static PyObject *ubus_python_disconnect(PyObject *module, PyObject *args, PyObject *kwargs)
{
	if (!CONNECTED) {
		PyErr_Format(PyExc_RuntimeError, MSG_NOT_CONNECTED);
		return NULL;
	}

	PyObject* deregister = Py_True;
	static char *kwlist[] = {"deregister", NULL};
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O!", kwlist, &PyBool_Type, &deregister)){
		return NULL;
	}

	dispose_connection(PyObject_IsTrue(deregister));

	Py_INCREF(Py_None);
	return Py_None;
}

PyDoc_STRVAR(
	connect_doc,
	"connect(socket_path='" DEFAULT_SOCKET "')\n"
	"\n"
	"Establishes a connection to ubus.\n"
);

static PyObject *ubus_python_connect(PyObject *module, PyObject *args, PyObject *kwargs)
{
	if (CONNECTED) {
		PyErr_Format(PyExc_RuntimeError, MSG_ALREADY_CONNECTED);
		return NULL;
	}

	static char *kwlist[] = {"socket_path", NULL};
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|s", kwlist, &socket_path)){
		return NULL;
	}

	// Init object list
	python_alloc_list = PyList_New(0);
	if (!python_alloc_list) {
		return NULL;
	}

	// socket path
	if (!socket_path) {
		socket_path = strdup(DEFAULT_SOCKET);
		if (!socket_path) {
			PyErr_Format(PyExc_MemoryError, MSG_ALLOCATION_FAILS);
			return NULL;
		}
	} else {
		char *tmp = strdup(socket_path);
		if (!tmp) {
			PyErr_Format(PyExc_MemoryError, MSG_ALLOCATION_FAILS);
			return NULL;
		}
		socket_path = tmp;
	}

	// Init event listner array
	listeners = NULL;
	listerners_size = 0;

	// Init objects array
	objects = NULL;
	objects_size = 0;

	// Connect to ubus
	ctx = ubus_connect(socket_path);
	if (!ctx) {
		PyErr_Format(
				PyExc_IOError,
				"Failed to connect to the ubus socket '%s'\n", socket_path
		);
		dispose_connection(true);
		return NULL;
	}
	ubus_add_uloop(ctx);
	memset(&python_buf, 0, sizeof(python_buf));

	return prepare_bool(true);
}

PyDoc_STRVAR(
	get_connected_doc,
	"get_connected()\n"
	"\n"
	"Determines whether we are connected to ubus.\n"
	":return: True if connected, False otherwise.\n"
	":rtype: bool \n"
);

static PyObject *ubus_python_get_connected(PyObject *module, PyObject *args, PyObject *kwargs)
{
	return prepare_bool(CONNECTED);
}

PyDoc_STRVAR(
	get_socket_path_doc,
	"get_socket_path()\n"
	"\n"
	"Gets socket path for the current connection.\n"
	":return: path to socket if connected, None otherwise.\n"
	":rtype: bool or str \n"
);

static PyObject *ubus_python_get_socket_path(PyObject *module, PyObject *args, PyObject *kwargs)
{
	if (socket_path) {
		return PyUnicode_FromString(socket_path);
	} else {
		Py_INCREF(Py_None);
		return Py_None;
	}
}

PyDoc_STRVAR(
	connect_send_doc,
	"send(event, data)\n"
	"\n"
	"Send an event via ubus.\n"
	"\n"
	":param event: ubus event which will be used \n"
	":type event: str\n"
	":param data: python object which can be serialized to json \n"
	":type data: dict or list \n"
	":return: True on success, False otherwise \n"
	":rtype: bool \n"
);

static PyObject *ubus_python_send(PyObject *module, PyObject *args, PyObject *kwargs)
{
	if (!CONNECTED) {
		PyErr_Format(PyExc_RuntimeError, MSG_NOT_CONNECTED);
		return NULL;
	}

	char *event = NULL;
	PyObject *data = NULL;
	static char *kwlist[] = {"event", "data",  NULL};
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "sO", kwlist, &event, &data)){
		return NULL;
	}

	// Call python function json.dumps
	PyObject *json_str = perform_json_function(DUMPS, data);
	if (!json_str) {
		return NULL;
	}

	// put json string into buffer
	blob_buf_init(&python_buf, 0);
	bool res = blobmsg_add_json_from_string(&python_buf, PyUnicode_AsUTF8(json_str));
	Py_DECREF(json_str);
	if (!res) {
		PyErr_Format(PyExc_TypeError, MSG_JSON_TO_UBUS_FAILED);
		return NULL;
	}

	int retval = ubus_send_event(ctx, event, python_buf.head);
	return prepare_bool(!retval);
}

static void ubus_python_event_handler(struct ubus_context *ctx, struct ubus_event_handler *ev,
			const char *type, struct blob_attr *msg)
{
	PyGILState_STATE gstate = PyGILState_Ensure();

	// Prepare event
	PyObject *event = PyUnicode_FromString(type);
	if (!event) {
		goto event_handler_cleanup0;
	}

	// Prepare json data
	char *str = blobmsg_format_json(msg, true);
	if (!str) {
		goto event_handler_cleanup1;
	}
	PyObject *data = PyUnicode_FromString(str);
	free(str);
	if (!data) {
		goto event_handler_cleanup1;
	}

	// Call python function json.loads
	PyObject *data_object = perform_json_function(LOADS, data);
	if (!data_object) {
		goto event_handler_cleanup2;
	}
	
	// Get PyObject callback
	ubus_Listener *listener = container_of(ev, ubus_Listener, handler);

	// Trigger callback
	PyObject *callback_arglist = Py_BuildValue("(O, O)", event, data_object);
	if (!callback_arglist) {
		goto event_handler_cleanup3;
	}

	PyObject *result = PyObject_CallObject(listener->callback, callback_arglist);
	if (result) {
		Py_DECREF(result);  // result of the callback is quite useless
	} else {
		PyErr_Print();
	}
	Py_DECREF(callback_arglist);

event_handler_cleanup3:
	Py_DECREF(data_object);
event_handler_cleanup2:
	Py_DECREF(data);
event_handler_cleanup1:
	Py_DECREF(event);

event_handler_cleanup0:
	// Clear python exceptions
	PyErr_Clear();

	PyGILState_Release(gstate);
}

PyDoc_STRVAR(
	connect_listen_doc,
	"listen(event, ...)\n"
	"\n"
	"Adds a listener on ubus events.\n"
	"\n"
	":param event: tuple contaning event string and a callback (str, callable) \n"
	":type event: tuple\n"
);

static PyObject *ubus_python_listen(PyObject *module, PyObject *args, PyObject *kwargs)
{
	if (!CONNECTED) {
		PyErr_Format(PyExc_RuntimeError, MSG_NOT_CONNECTED);
		return NULL;
	}

	args = PySequence_Fast(args, "expected a sequence");
	int len = PySequence_Size(args);
	if (!len) {
		PyErr_Format(PyExc_TypeError, "You need to set at least one event.");
		goto listen_error1;
	}

	if (!PyTuple_Check(args)) {
		PyErr_Format(PyExc_TypeError, "Tuple of (event, callback) expected.");
		goto listen_error1;
	}

	// Test whether the arguments are valid
	for (int i = 0; i < len; i++) {
		PyObject *item = PySequence_Fast_GET_ITEM(args, i);
		// Test tuple
		if (!PyTuple_Check(item)) {
			PyErr_Format(PyExc_TypeError, MSG_LISTEN_TUPLE_EXPECTED);
			goto listen_error1;
		}
		PyObject *item_tuple = PySequence_Fast(item, MSG_LISTEN_TUPLE_EXPECTED);
		if (!item_tuple) {
			PyErr_Format(PyExc_MemoryError, "Failed to obtain tuple item");
			goto listen_error1;
		}
		if (!PyTuple_Check(item_tuple) || PySequence_Size(item_tuple) != 2) {
			PyErr_Format(PyExc_TypeError, MSG_LISTEN_TUPLE_EXPECTED);
			Py_DECREF(item_tuple);
			Py_DECREF(args);
			goto listen_error1;
		}

		// Test types
		if (!PyStr_Check(PyTuple_GET_ITEM(item_tuple, 0))
					|| !PyCallable_Check(PyTuple_GET_ITEM(item_tuple, 1))) {
			PyErr_Format(PyExc_TypeError, MSG_LISTEN_TUPLE_EXPECTED);
			Py_DECREF(item_tuple);
			goto listen_error1;
		}
		Py_DECREF(item_tuple);
	}

	// add callbacks
	for (int i = 0; i < len; i++) {
		PyObject *item = PySequence_Fast_GET_ITEM(args, i);
		PyObject *item_tuple = PySequence_Fast(item, MSG_LISTEN_TUPLE_EXPECTED);
		if (!item_tuple) {
			PyErr_Format(PyExc_MemoryError, "Failed to obtain tuple item");
			goto listen_error1;
		}
		PyObject *event = PyTuple_GET_ITEM(item_tuple, 0);
		PyObject *callback = PyTuple_GET_ITEM(item_tuple, 1);
		// Keep event and callback references
		if (PyList_Append(python_alloc_list, event) || PyList_Append(python_alloc_list, callback)) {
			Py_DECREF(item_tuple);
			goto listen_error1;
		}
		Py_DECREF(item_tuple);

		// prepare event listener
		ubus_Listener *listener = calloc(1, sizeof(ubus_Listener));
		if (!listener) {
			PyErr_Format(PyExc_MemoryError, MSG_ALLOCATION_FAILS);
			Py_DECREF(item_tuple);
			goto listen_error1;
		}

		listener->handler.cb = ubus_python_event_handler;
		listener->callback = callback;

		ubus_Listener **new_listeners = realloc(listeners,
			(listerners_size + 1) * sizeof(*listeners));
		if (!new_listeners) {
			free(listener);
			goto listen_error1;
		}
		listeners = new_listeners;
		listeners[listerners_size++] = listener;

		// register event handler
		int retval = ubus_register_event_handler(ctx, &listener->handler, PyUnicode_AsUTF8(event));
		if (retval != UBUS_STATUS_OK) {
			listerners_size--;
			free(listener);
		}
	}

	Py_DECREF(args);

	Py_INCREF(Py_None);
	return Py_None;

listen_error1:
	Py_DECREF(args);
	return NULL;
}

static void ubus_python_timeout_handler(struct uloop_timeout *timeout) {
	uloop_end();
}

PyDoc_STRVAR(
	connect_loop_doc,
	"loop(timeout=-1)\n"
	"\n"
	"Enters a loop and processes events.\n"
	"\n"
	":param timeout: loop timeout in ms (if lower than zero then it will run forever) \n"
	":type timeout: int\n"
);

static PyObject *ubus_python_loop(PyObject *module, PyObject *args, PyObject *kwargs)
{
	if (!CONNECTED) {
		PyErr_Format(PyExc_RuntimeError, MSG_NOT_CONNECTED);
		return NULL;
	}

	int timeout = -1;
	static char *kwlist[] = {"timeout", NULL};
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|i", kwlist, &timeout)){
		return NULL;
	}

	Py_BEGIN_ALLOW_THREADS
	if (timeout == 0) {
		// process events directly without uloop
		ubus_handle_event(ctx);
	} else {
		uloop_init();
		struct uloop_timeout u_timeout;
		if (timeout > 0) {
			// prepare for timeout
			memset(&u_timeout, 0, sizeof(u_timeout));
			u_timeout.cb = ubus_python_timeout_handler;
			uloop_timeout_set(&u_timeout, timeout);  // Timeout to seconds
		}
		uloop_run();
		if (timeout > 0) {
			uloop_timeout_cancel(&u_timeout);
		}
	}
	Py_END_ALLOW_THREADS

	Py_INCREF(Py_None);
	return Py_None;
}

bool test_policies(const struct blobmsg_policy *policies, int n_policies, struct blob_attr *args)
{
	struct blob_attr *cur;
	int idx = 0, passed_count = 0;

	blob_for_each_attr(cur, args, idx) {
		const char *name = blobmsg_name(cur);
		int type = blobmsg_type(cur);
		int pol_idx;

		// Iterate through policies
		for (pol_idx = 0; pol_idx < n_policies; pol_idx++) {

			if (!strcmp(name, policies[pol_idx].name)) {
				passed_count += 1;
				int pol_type = policies[pol_idx].type;
				if (pol_type != BLOBMSG_TYPE_UNSPEC && pol_type != type) {
					return false;
				}
				break;
			}
		}

		// Policy was not found
		if (pol_idx >= n_policies) {
			return false;
		}
	}

	// All attributes are present and checked
	return passed_count == n_policies;
}

static int ubus_python_method_handler(struct ubus_context *ctx, struct ubus_object *obj,
		struct ubus_request_data *req, const char *method,
		struct blob_attr *msg)
{
	// Check whether method signature matches
	int method_idx;
	for (method_idx = 0; method_idx < obj->n_methods; ++method_idx) {
		if (!strcmp(obj->methods[method_idx].name, method)) {
			break;
		}
	}
	if (method_idx >= obj->n_methods) {
		// Can't find method
		return UBUS_STATUS_UNKNOWN_ERROR;
	}
	if (!test_policies(obj->methods[method_idx].policy, obj->methods[method_idx].n_policy, msg)) {
		return UBUS_STATUS_INVALID_ARGUMENT;
	}

	PyGILState_STATE gstate = PyGILState_Ensure();

	int retval = UBUS_STATUS_OK;
	// Get python method
	PyObject *methods = container_of(obj, ubus_Object, object)->methods;
	PyObject *python_method = PyDict_GetItemString(methods, method);
	if (!python_method) {
		retval = UBUS_STATUS_METHOD_NOT_FOUND;
		goto method_handler_exit;
	}

	// prepare json data
	char *str = blobmsg_format_json(msg, true);
	if (!str) {
		retval = UBUS_STATUS_UNKNOWN_ERROR;
		goto method_handler_exit;
	}
	PyObject *data = PyUnicode_FromString(str);
	free(str);
	if (!data) {
		retval = UBUS_STATUS_UNKNOWN_ERROR;
		goto method_handler_exit;
	}

	// Call python function json.loads
	PyObject *data_object = perform_json_function(LOADS, data);
	if (!data_object) {
		retval = UBUS_STATUS_UNKNOWN_ERROR;
		goto method_handler_cleanup1;
	}

	PyObject *handler = PyObject_CallObject((PyObject *)&ubus_ResponseHandlerType, NULL);
	if (!handler) {
		PyErr_Print();
		goto method_handler_cleanup2;
	}
	((ubus_ResponseHandler *)handler)->req = req;
	((ubus_ResponseHandler *)handler)->ctx = ctx;

	// Trigger method
	PyObject *callback_arglist = Py_BuildValue("(O, O)", handler, data_object);
	if (!callback_arglist) {
		retval = UBUS_STATUS_UNKNOWN_ERROR;
		goto method_handler_cleanup3;
	}
	PyObject *callable = PyDict_GetItemString(python_method, "method");
	PyObject *result = PyObject_CallObject(callable, callback_arglist);
	Py_DECREF(callback_arglist);
	if (!result) {
		PyErr_Print();
		retval = UBUS_STATUS_UNKNOWN_ERROR;
	} else {
		Py_DECREF(result);  // we don't care about the result
	}

method_handler_cleanup3:
	// NULLify the structures so that using this structure will we useless if a reference
	// is left outside the callback code
	((ubus_ResponseHandler *)handler)->req = NULL;
	((ubus_ResponseHandler *)handler)->ctx = NULL;
	Py_DECREF(handler);
method_handler_cleanup2:
	Py_DECREF(data_object);
method_handler_cleanup1:
	Py_DECREF(data);
method_handler_exit:

	// Clear python exceptions
	PyErr_Clear();

	PyGILState_Release(gstate);

	return retval;
}

static bool test_methods_argument(PyObject *methods)
{
	if (!methods) {
		return false;
	}

	PyObject *method_name = NULL, *value = NULL;
	Py_ssize_t pos = 0;
	// Iterate through methods
	while(PyDict_Next(methods, &pos, &method_name, &value)) {
		// Test name
		if (!PyStr_Check(method_name)) {
			return false;
		}
		if (!PyDict_Check(value)) {
			return false;
		}

		// Dict should contain only two elemnts - 'signature' and 'method'
		if (PyDict_Size(value) != 2) {
				return false;
		}

		// Test signature
		PyObject *signature = PyDict_GetItemString(value, "signature");
		if (!signature || !PyDict_Check(signature)) {
			return false;
		}
		Py_ssize_t sig_pos = 0;
		PyObject *signature_name = NULL, *signature_type = NULL;
		while (PyDict_Next(signature, &sig_pos, &signature_name, &signature_type)) {
			if (!PyStr_Check(signature_name)) {
				return false;
			}
			if (!PyInt_Check(signature_type)) {
				return false;
			}
			int type = PyLong_AsLong(signature_type);
			if (type < 0 || type > BLOBMSG_TYPE_LAST) {  // indexed from 0
				return false;
			}
		}

		// Test callable
		PyObject *method = PyDict_GetItemString(value, "method");
		if (!method || !PyCallable_Check(method)) {
			return false;
		}
	}

	return true;
}

PyDoc_STRVAR(
	connect_add_doc,
	"add(object_name, methods)\n"
	"\n"
	"Adds an object to ubus.\n"
	"methods should look like this: \n"
	"{ \n"
	"	<method_name>: {'signature': <method_signature>, 'method': <callable>} \n"
	"} \n"
	"\n"
	"{ \n"
	"	test: {'signature': {'argument1': BLOBMSG_TYPE_STRING}, 'method': my_callback} \n"
	"} \n"
	"\n"
	":param object_name: the name of the object which will be present on ubus \n"
	":type object_name: str\n"
	":param methods: {<method_name>: callable} where callable signature is (request, msg) \n"
	":type methods: dict\n"
);

static PyObject *ubus_python_add(PyObject *module, PyObject *args, PyObject *kwargs)
{
	if (!CONNECTED) {
		PyErr_Format(PyExc_RuntimeError, MSG_NOT_CONNECTED);
		return NULL;
	}

	// arguments
	PyObject *object_name = NULL;
	PyObject *methods= NULL;
	static char *kwlist[] = {"object_name", "methods",  NULL};
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "OO", kwlist, &object_name, &methods)){
		return NULL;
	}

	// test arguments
	if (!PyStr_Check(object_name)) {
		PyErr_Format(PyExc_TypeError, MSG_ADD_SIGNATURE_INVALID);
		return NULL;
	}

	if (!test_methods_argument(methods)) {
		PyErr_Format(PyExc_TypeError, MSG_ADD_SIGNATURE_INVALID);
		return NULL;
	}

	// allocate the object
	ubus_Object *object = calloc(1, sizeof(ubus_Object));
	if (!object) {
		PyErr_Format(PyExc_MemoryError, MSG_ALLOCATION_FAILS);
		return NULL;
	}
	object->methods = methods;

	// set the object
	object->object.name = PyUnicode_AsUTF8(object_name);
	object->object.n_methods = PyDict_Size(methods);

	if (object->object.n_methods > 0) {
		struct ubus_method *ubus_methods = calloc(object->object.n_methods, sizeof(struct ubus_method));
		if (!ubus_methods) {
			free(object);
			PyErr_Format(PyExc_MemoryError, MSG_ALLOCATION_FAILS);
			return NULL;
		}

		PyObject *method_name = NULL, *value = NULL;
		Py_ssize_t pos = 0;
		// Iterate through methods
		for (int i = 0; PyDict_Next(methods, &pos, &method_name, &value); i++) {
			ubus_methods[i].name = PyUnicode_AsUTF8(method_name);
			ubus_methods[i].handler = ubus_python_method_handler;

			// alocate and set policy objects
			PyObject *signature = PyDict_GetItemString(value, "signature");
			Py_ssize_t signature_size = PyDict_Size(signature);
			struct blobmsg_policy *policy = calloc(signature_size, sizeof(struct blobmsg_policy));
			if (!policy) {
				// dealloc allocated data
				free_ubus_object(object);
				PyErr_Format(PyExc_MemoryError, MSG_ALLOCATION_FAILS);
				return NULL;
			}
			Py_ssize_t sig_pos = 0;
			PyObject *signature_name = NULL, *signature_type = NULL;
			for (int j = 0; PyDict_Next(signature, &sig_pos, &signature_name, &signature_type); j++) {
				policy[j].name = PyUnicode_AsUTF8(signature_name);
				policy[j].type = PyLong_AsLong(signature_type);
			}
			ubus_methods[i].policy = policy;
			ubus_methods[i].n_policy = signature_size;
		}

		// assign methods
		object->object.methods = ubus_methods;
	}

	object->object.type = calloc(1, sizeof(struct ubus_object_type));
	if (!object->object.type) {
		free_ubus_object(object);
		PyErr_Format(PyExc_MemoryError, MSG_ALLOCATION_FAILS);
		return NULL;
	}
	object->object.type->name = PyUnicode_AsUTF8(object_name);
	object->object.type->methods = object->object.methods;
	object->object.type->n_methods = object->object.n_methods;

	// add object to object array to be deallocated later
	ubus_Object **new_objects = realloc(objects,
			(objects_size + 1) * sizeof(*objects));
	if (!new_objects) {
		// dealloc the object
		free_ubus_object(object);
		PyErr_Format(PyExc_MemoryError, MSG_ALLOCATION_FAILS);
		return NULL;
	}
	objects = new_objects;
	objects[objects_size++] = object;

	int ret = ubus_add_object(ctx, &object->object);
	if (ret) {
		// deallocate object on failure
		objects_size--;  // no need to realloc the whole array
		free_ubus_object(object);

		PyErr_Format(
				PyExc_RuntimeError,
				"ubus error occured: %s", ubus_strerror(ret)
		);
		return NULL;
	}

	// put arguments into alloc list (used for reference counting)
	if (PyList_Append(python_alloc_list, object_name)) {
		ubus_remove_object(ctx, &object->object);
		free_ubus_object(object);
		return NULL;
	}

	if (PyList_Append(python_alloc_list, methods)) {
		ubus_remove_object(ctx, &object->object);
		free_ubus_object(object);
		PyEval_CallMethod(python_alloc_list, "pop", "");
		return NULL;
	}

	Py_INCREF(Py_None);
	return Py_None;
}

static void ubus_python_objects_handler(struct ubus_context *c, struct ubus_object_data *o, void *p)
{
	// should be a single instance for all the objects
	PyObject *objects = (PyObject *)p;

	PyObject *str_signatures = PyUnicode_FromString("{");
	if (!str_signatures) {
		return;
	}

	bool first = true;
	if (o->signature) {
		struct blob_attr *cur;
		int rem = 0;
		blob_for_each_attr(cur, o->signature, rem) {
			char *s = blobmsg_format_json(cur, false);
			if (!s) {
				goto object_handler_cleanup;
			}
			PyObject *str_signature = NULL;
			if (first) {
				first = false;
				str_signature = PyUnicode_FromString(s);
			} else {
				str_signature = PyUnicode_FromFormat(" ,%s", s);
			}
			free(s);
			if (!str_signature) {
				goto object_handler_cleanup;
			}
			PyObject *tmp = PyUnicode_Concat(str_signatures, str_signature);
			Py_DECREF(str_signature);
			Py_DECREF(str_signatures);
			if (!tmp) {
				return;
			}
			str_signatures = tmp;
		}
	}

	PyObject *closing_bracket = PyUnicode_FromString("}");
	PyObject *tmp = PyUnicode_Concat(str_signatures, closing_bracket);
	Py_DECREF(closing_bracket);
	Py_DECREF(str_signatures);

	if (!tmp) {
		return;
	}
	str_signatures = tmp;

	// convert json string to json
	PyObject *json_signatures = perform_json_function(LOADS, str_signatures);
	if (!json_signatures) {
		goto object_handler_cleanup;
	}

	// Add it to dict object
	PyObject *path = PyUnicode_FromString(o->path);
	if (!path) {
		goto object_handler_cleanup;
	}
	PyDict_SetItem(objects, path, json_signatures);  // we don't care about retval here
	Py_DECREF(path);

object_handler_cleanup:
	Py_DECREF(str_signatures);

	// Clear python exceptions
	PyErr_Clear();
}

PyDoc_STRVAR(
	connect_objects_doc,
	"objects(path='*')\n"
	"\n"
	"Prints all objects present on ubus\n"
	"\n"
	":param path: only object which match the given path \n"
	":type path: str\n"
	":return: {<object_path>: {{<function_name>: <function_signature>}, ...}, ...} \n"
	":rtype: dict\n"
);

static PyObject *ubus_python_objects(PyObject *module, PyObject *args, PyObject *kwargs)
{
	if (!CONNECTED) {
		PyErr_Format(PyExc_RuntimeError, MSG_NOT_CONNECTED);
		return NULL;
	}

	char *ubus_path = NULL;
	static char *kwlist[] = {"path", NULL};
	if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|s", kwlist, &ubus_path)){
		return NULL;
	}

	ubus_path = ubus_path ? ubus_path : "*";

	PyObject *res = PyDict_New();
	if (!res) {
		return NULL;
	}

	int retval = ubus_lookup(ctx, ubus_path, ubus_python_objects_handler, res);
	switch (retval) {
		case UBUS_STATUS_OK:
		case UBUS_STATUS_NOT_FOUND:
			break;
		default:
			Py_DECREF(res);
			PyErr_Format(
					PyExc_RuntimeError,
					"ubus error occured: %s", ubus_strerror(retval)
			);
			return NULL;
	}

	return res;
}

static void ubus_python_call_handler(struct ubus_request *req, int type, struct blob_attr *msg)
{
	assert(type == UBUS_MSG_DATA);

	PyObject **results = (PyObject **)req->priv;
	if (!*results) {
		// error has occured in some previous call -> exit
		return;
	}

	if (!msg) {
		PyErr_Format(PyExc_RuntimeError, "No data in call hander");
		goto call_handler_cleanup;
	}

	// convert message do python json object
	char *str = blobmsg_format_json(msg, true);
	if (!str) {
		PyErr_Format(PyExc_RuntimeError, MSG_JSON_FROM_UBUS_FAILED);
		goto call_handler_cleanup;
	}
	PyObject *data = PyUnicode_FromString(str);
	free(str);
	if (!data) {
		goto call_handler_cleanup;
	}
	PyObject *data_object = perform_json_function(LOADS, data);
	Py_DECREF(data);
	if (!data_object) {
		goto call_handler_cleanup;
	}

	// append to results
	int failed = PyList_Append(*results, data_object);
	Py_DECREF(data_object);
	if (failed) {
		goto call_handler_cleanup;
	}

	return;

	call_handler_cleanup:

	// clear the result
	Py_DECREF(*results);
	results = NULL;
}

PyDoc_STRVAR(
	connect_call_doc,
	"call(object, method, arguments, timeout=0)\n"
	"\n"
	"Calls object's method on ubus.\n"
	"\n"
	":param object: name of the object\n"
	":type object: str\n"
	":param method: name of the method\n"
	":type method: str\n"
	":param arguments: arguments of the method (should be JSON serialisable).\n"
	":type argument: dict\n"
	":param timeout: timeout in ms (0 = wait forever)\n"
	":type timeout: int\n"
);

static PyObject *ubus_python_call(PyObject *module, PyObject *args, PyObject *kwargs)
{
	if (!CONNECTED) {
		PyErr_Format(PyExc_RuntimeError, MSG_NOT_CONNECTED);
		return NULL;
	}

	char *object = NULL, *method = NULL;
	int timeout = 0;
	PyObject *arguments = NULL;
	static char *kwlist[] = {"object", "method", "arguments", "timeout", NULL};
	if (!PyArg_ParseTupleAndKeywords(
				args, kwargs, "ssO|i", kwlist, &object, &method, &arguments, &timeout)){
		return NULL;
	}
	if (timeout < 0) {
		PyErr_Format(PyExc_TypeError, "timeout can't be lower than 0");
		return NULL;
	}

	uint32_t id = 0;
	int retval = ubus_lookup_id(ctx, object, &id);
	if (retval != UBUS_STATUS_OK) {
		PyErr_Format(PyExc_RuntimeError, "Object '%s' was not found.", object);
		return NULL;
	}

	// Call python function json.dumps
	PyObject *json_arguments = perform_json_function(DUMPS, arguments);
	if (!json_arguments) {
		return NULL;
	}

	// put data into buffer
	blob_buf_init(&python_buf, 0);
	bool res = blobmsg_add_json_from_string(&python_buf, PyUnicode_AsUTF8(json_arguments));
	Py_DECREF(json_arguments);
	if (!res) {
		PyErr_Format(PyExc_TypeError, MSG_JSON_TO_UBUS_FAILED);
		return NULL;
	}

	PyObject *results = PyList_New(0);
	if (!results) {
		return NULL;
	}

	retval = ubus_invoke(
			ctx, id, method, python_buf.head, ubus_python_call_handler, &results, timeout);

	if (retval != UBUS_STATUS_OK) {
		Py_XDECREF(results);
		PyErr_Format(
				PyExc_RuntimeError,
				"ubus error occured: %s", ubus_strerror(retval)
		);
		return NULL;
	}

	// Note that results might be NULL indicating that something went wrong in the handler
	return results;
}

static PyMethodDef ubus_methods[] = {
	{"disconnect", (PyCFunction)ubus_python_disconnect, METH_VARARGS|METH_KEYWORDS, disconnect_doc},
	{"connect", (PyCFunction)ubus_python_connect, METH_VARARGS|METH_KEYWORDS, connect_doc},
	{"get_connected", (PyCFunction)ubus_python_get_connected, METH_NOARGS, get_connected_doc},
	{"get_socket_path", (PyCFunction)ubus_python_get_socket_path, METH_NOARGS, get_socket_path_doc},
	{"send", (PyCFunction)ubus_python_send, METH_VARARGS|METH_KEYWORDS, connect_send_doc},
	{"listen", (PyCFunction)ubus_python_listen, METH_VARARGS, connect_listen_doc},
	{"loop", (PyCFunction)ubus_python_loop, METH_VARARGS|METH_KEYWORDS, connect_loop_doc},
	{"add", (PyCFunction)ubus_python_add, METH_VARARGS|METH_KEYWORDS, connect_add_doc},
	{"objects", (PyCFunction)ubus_python_objects, METH_VARARGS|METH_KEYWORDS, connect_objects_doc},
	{"call", (PyCFunction)ubus_python_call, METH_VARARGS|METH_KEYWORDS, connect_call_doc},
	{NULL}
};


#if PY_MAJOR_VERSION >= 3
PyMODINIT_FUNC PyInit_ubus(void)
#else
void initubus(void)
#endif
{

	if (PyType_Ready(&ubus_ResponseHandlerType)) {
		goto init_ubus_exit_fail;
	}

	json_module = PyImport_ImportModule("json");
	if (!json_module) {
		goto init_ubus_exit_fail;
	}

	PyObject *module = ubus_python_module_init();
	if (!module) {
		goto init_ubus_exit_fail;
	}

	struct module_state *st = GETSTATE(module);
	st->error = PyErr_NewException("ubus.Error", NULL, NULL);
	if (st->error == NULL) {
		Py_DECREF(module);
		goto init_ubus_exit_fail;
	}

	Py_INCREF(&ubus_ResponseHandlerType);
	PyModule_AddObject(module, "__ResponseHandler", (PyObject *)&ubus_ResponseHandlerType);

	/* export ubus json types */
	PyModule_AddIntMacro(module, BLOBMSG_TYPE_UNSPEC);
	PyModule_AddIntMacro(module, BLOBMSG_TYPE_ARRAY);
	PyModule_AddIntMacro(module, BLOBMSG_TYPE_TABLE);
	PyModule_AddIntMacro(module, BLOBMSG_TYPE_STRING);
	PyModule_AddIntMacro(module, BLOBMSG_TYPE_INT64);
	PyModule_AddIntMacro(module, BLOBMSG_TYPE_INT32);
	PyModule_AddIntMacro(module, BLOBMSG_TYPE_INT16);
	PyModule_AddIntMacro(module, BLOBMSG_TYPE_INT8);
	PyModule_AddIntMacro(module, BLOBMSG_TYPE_DOUBLE);
	PyModule_AddIntMacro(module, BLOBMSG_TYPE_BOOL);

#if PY_MAJOR_VERSION >= 3
	return module;
#endif

init_ubus_exit_fail:
#if PY_MAJOR_VERSION >= 3
	return NULL;
#else
	return;
#endif
}
