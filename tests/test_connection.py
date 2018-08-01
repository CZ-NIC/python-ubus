# -*- coding: utf-8 -*-

import time
import pytest
import ubus
import sys

from .fixtures import (
    event_sender,
    call_for_object,
    calls_extensive,
    disconnect_after,
    ubusd_test,
    registered_objects,
    responsive_object,
    UBUSD_TEST_SOCKET_PATH,
)

stored_reference_counts = None


class CheckRefCount(object):

    def __init__(self, *objects):
        self.objects = objects

    def __enter__(self):
        self.stored_reference_counts = [sys.getrefcount(e) for e in self.objects]

    def __exit__(self, exc_type, *args):

        if exc_type:
            # don't count references when an exception occured
            return

        objects = self.objects
        current = [sys.getrefcount(e) for e in objects]
        stored = self.stored_reference_counts
        for (obj, old, new) in zip(objects, stored, current):
            assert old == new, "Reference count for '%s' mismatch (%d!=%d)" % (obj, old, new)


def test_socket_missing(ubusd_test):
    path = "/non/existing/path"

    with CheckRefCount(path):
        with pytest.raises(IOError):
            ubus.connect(socket_path="/non/existing/path")


def test_connect_and_disconnect(ubusd_test, disconnect_after):
    path = UBUSD_TEST_SOCKET_PATH

    with CheckRefCount(path):

        assert ubus.get_connected() is False
        assert ubus.get_socket_path() is None

        ubus.connect(socket_path=path)
        assert ubus.get_socket_path() == path
        assert ubus.get_connected() is True

        assert ubus.disconnect() is None
        with pytest.raises(RuntimeError):
            ubus.disconnect()
        assert ubus.get_connected() is False
        assert ubus.get_socket_path() is None

        ubus.connect(socket_path=path)
        with pytest.raises(RuntimeError):
            ubus.connect(socket_path=path)
        assert ubus.get_socket_path() == path
        assert ubus.get_connected() is True

        assert ubus.disconnect() is None

        assert ubus.get_connected() is False
        assert ubus.get_socket_path() is None


def test_send_failed(ubusd_test, disconnect_after):
    path = UBUSD_TEST_SOCKET_PATH

    with CheckRefCount(path):

        with pytest.raises(RuntimeError):
            ubus.send("disconnected", {})

        ubus.connect(socket_path=path)
        with pytest.raises(TypeError):
            ubus.send()

        with pytest.raises(TypeError):
            ubus.send({}, {})

        with pytest.raises(TypeError):
            ubus.send("", "")

        with pytest.raises(TypeError):
            ubus.send("", {}, {})

        class NonSerializable(object):
            pass

        with pytest.raises(TypeError):
            ubus.send("", NonSerializable())

        ubus.disconnect()


def test_send_succeeded(ubusd_test, disconnect_after):
    path = UBUSD_TEST_SOCKET_PATH
    test_dict = dict(a=5, b="True", c=False)

    with CheckRefCount(path, test_dict):

        ubus.connect(socket_path=path)

        assert ubus.send("normal", test_dict)
        assert ubus.send("*", test_dict)
        assert ubus.send("", test_dict)

        ubus.disconnect()


def test_loop(ubusd_test, disconnect_after):
    path = UBUSD_TEST_SOCKET_PATH

    time1 = 200
    time2 = 1
    time3 = 50
    time4 = 2 ** 65
    time5 = None
    time6 = "5"
    time7 = 0

    with CheckRefCount(path, time1, time2, time3, time4, time6, time7):

        with pytest.raises(RuntimeError):
            ubus.loop(time1)

        ubus.connect(socket_path=path)
        assert ubus.loop(time1) is None
        assert ubus.loop(time2) is None
        assert ubus.loop(time3) is None

        with pytest.raises(OverflowError):
            ubus.loop(time4)

        with pytest.raises(TypeError):
            ubus.loop(time5)

        with pytest.raises(TypeError):
            ubus.loop(time6)

        assert ubus.loop(time7) is None

        ubus.disconnect()


def test_listen_failed(ubusd_test, disconnect_after):
    path = UBUSD_TEST_SOCKET_PATH

    obj1 = {}
    obj2 = ("a", lambda x, y: (x, y))
    obj3 = ("b", None)
    obj4 = ({}, lambda x, y: (x, y))
    obj5 = ("b", lambda x, y: (x, y))

    with CheckRefCount(path, obj1, obj2, obj3, obj4, obj5):

        with pytest.raises(RuntimeError):
            ubus.listen(obj1, obj1)

        ubus.connect(socket_path=path)

        with pytest.raises(TypeError):
            ubus.listen(obj1, obj1)

        with pytest.raises(TypeError):
            ubus.send(obj2, obj3)

        with pytest.raises(TypeError):
            ubus.send(obj4, obj5)

        with pytest.raises(TypeError):
            ubus.listen()

        ubus.disconnect()


def test_listen(ubusd_test, event_sender, disconnect_after):
    listen_test = {"passed": False, "passed2": False}

    def set_result(event, data):
        assert event == "event_sender"
        assert data == dict(a="b", c=3, d=False)
        listen_test["passed"] = True

    def test1(event, data):
        assert event == "event_sender"
        assert data == dict(a="b", c=3, d=False)
        listen_test["passed2"] = True

    path = UBUSD_TEST_SOCKET_PATH
    timeout = 300
    event_name = "event_sender"

    with CheckRefCount(path, time, event_name, test1):

        ubus.connect(socket_path=path)
        ubus.listen((event_name, test1), (event_name, set_result))
        ubus.listen((event_name, test1))

        del set_result

        ubus.loop(timeout)
        assert listen_test["passed"]
        assert listen_test["passed2"]

        listen_test = {"passed": False, "passed2": False}
        ubus.loop(timeout)
        assert listen_test["passed"]
        assert listen_test["passed2"]

        ubus.disconnect()


def test_add_object_failed(ubusd_test, registered_objects, disconnect_after):
    path = UBUSD_TEST_SOCKET_PATH

    def fake(*args):
        pass

    type_errors = [
        [],
        ("name", ),
        ("name", {}, False),
        ("name", {"test": []}),
        ("name", {"test": 5}),
        ("name", {"test": {"method": 5, "signature": {}}}),
        ("name", {"test": {"method": fake}}),
        ("name", {"test": {"method": fake, "signature": {}, "another": 5}}),
    ]

    runtime_errors = [
        ("registered_object1", {"test": {"method": fake, "signature": {}}}),
        ("registered_object2", {}),
    ]

    with CheckRefCount(path, fake, *(type_errors + runtime_errors)):

        with pytest.raises(RuntimeError):
            ubus.add(*type_errors[0])

        ubus.connect(socket_path=path)

        for wrong_args in type_errors:
            with pytest.raises(TypeError):
                ubus.add(*wrong_args)

        for wrong_args in runtime_errors:
            with pytest.raises(RuntimeError):
                ubus.add(*wrong_args)

        ubus.disconnect()
        del wrong_args


def test_add_object(ubusd_test, disconnect_after):
    path = UBUSD_TEST_SOCKET_PATH

    def fake(*args):
        pass

    arguments1 = ("new_object1", {})
    arguments2 = ("new_object2", {"test": {"method": fake, "signature": {}}})
    arguments3 = ("new_object3", {
        "test1": {"method": fake, "signature": dict(arg1=3)},
        "test2": {"method": fake, "signature": dict(arg2=2)},
    })

    with CheckRefCount(path, fake, *(arguments1 + arguments2 + arguments3)):

        ubus.connect(socket_path=path)

        assert ubus.add(*arguments1) is None
        assert ubus.add(*arguments2) is None
        assert ubus.add(*arguments3) is None

        with pytest.raises(RuntimeError):
            ubus.add(*arguments1)

        ubus.disconnect()


def test_list_objects_failed(ubusd_test, registered_objects, disconnect_after):
    path = UBUSD_TEST_SOCKET_PATH

    type_errors = [
        ("more", "than", "one"),
        ({}, ),
        (True, ),
        (3, ),
        (None, ),
    ]

    with CheckRefCount(path, *(type_errors)):

        with pytest.raises(RuntimeError):
            ubus.objects()

        ubus.connect(socket_path=path)

        for wrong_args in type_errors:
            with pytest.raises(TypeError):
                ubus.objects(*wrong_args)

        ubus.disconnect()
        del wrong_args


def test_list_objects(ubusd_test, registered_objects, disconnect_after):
    path = UBUSD_TEST_SOCKET_PATH

    def fake(*args):
        pass

    new_object = ("new_object", {"new_method": {"method": fake, "signature": {}}})

    expected1 = {
        u"registered_object1": {
            u"method1": {},
            u"method2": {
                u"first": ubus.BLOBMSG_TYPE_STRING,
                u"second": ubus.BLOBMSG_TYPE_BOOL,
                u"third": ubus.BLOBMSG_TYPE_INT32,
            },
        },
        u"registered_object2": {},
        u"registered_object3": {
            u"method1": {},
        },
    }

    expected2 = {
        u"registered_object2": {},
    }

    expected3 = {
        u"registered_object1": {
            u"method1": {},
            u"method2": {
                u"first": ubus.BLOBMSG_TYPE_STRING,
                u"second": ubus.BLOBMSG_TYPE_BOOL,
                u"third": ubus.BLOBMSG_TYPE_INT32,
            },
        },
        u"registered_object2": {},
        u"registered_object3": {
            u"method1": {},
        },
        u"new_object": {
            u"new_method": {},
        },
    }

    expected4 = {
        u"new_object": {"new_method": {}},
    }

    with CheckRefCount(path, fake, expected1, expected2, expected3, expected4, *new_object):

        ubus.connect(socket_path=path)

        # All objects
        res1 = ubus.objects()
        assert res1 == expected1

        # Filtered objects
        res2 = ubus.objects("registered_object2")
        assert res2 == expected2

        # Append an object
        ubus.add(*new_object)

        # All objects + new
        res3 = ubus.objects()
        assert res3 == expected3

        # New object
        res4 = ubus.objects("new_object")
        assert res4 == expected4

        ubus.disconnect()
        del res1
        del res2
        del res3
        del res4


def test_list_objects_empty(ubusd_test, disconnect_after):
    path = UBUSD_TEST_SOCKET_PATH

    with CheckRefCount(path):
        ubus.connect(path)
        assert ubus.objects() == {}

        ubus.disconnect()


def test_reply_out_of_handler():
    data = {"this": "should fail"}

    with CheckRefCount(data):

        # should be called only within call
        handler = ubus.__ResponseHandler()
        with pytest.raises(RuntimeError):
            handler.reply(data)

        del handler


def test_reply_failed(ubusd_test, call_for_object, disconnect_after):
    path = UBUSD_TEST_SOCKET_PATH

    results = {e: {'data': None, 'exits': False} for e in range(1, 4)}

    def handler_fail1(handler, data):
        results[1]['data'] = data
        handler.reply()
        results[1]['exits'] = True

    def handler_fail2(handler, data):
        results[2]['data'] = data
        handler.reply(6)
        results[2]['exits'] = True

    def handler_fail3(handler, data):
        results[3]['data'] = data
        handler.reply({'data': 6}, {'fail': 'here'})
        results[3]['exits'] = True

    with CheckRefCount(path, results, handler_fail1, handler_fail2, handler_fail3):

        ubus.connect(path)
        ubus.add(
            "callee_object",
            {
                "method1": {"method": handler_fail1, "signature": {
                    "first": ubus.BLOBMSG_TYPE_INT32,
                }},
                "method2": {"method": handler_fail2, "signature": {
                    "second": ubus.BLOBMSG_TYPE_INT32,
                }},
                "method3": {"method": handler_fail3, "signature": {}},
            },
        )
        ubus.loop(500)

        assert results == {
            1: {'data': {'first': 1}, 'exits': False},
            2: {'data': {'second': 2}, 'exits': False},
            3: {'data': {}, 'exits': False},
        }

        ubus.disconnect()


def test_reply(ubusd_test, call_for_object, disconnect_after):
    path = UBUSD_TEST_SOCKET_PATH

    results = {e: {'data': None, 'exits': False} for e in range(1, 4)}

    def handler1(handler, data):
        results[1]['data'] = data
        handler.reply(data)
        results[1]['exits'] = True

    def handler2(handler, data):
        results[2]['data'] = data
        handler.reply(data)
        results[2]['exits'] = True

    def handler3(handler, data):
        results[3]['data'] = data
        handler.reply(data)
        results[3]['exits'] = True

    with CheckRefCount(path, results, handler1, handler2, handler3):

        ubus.connect(path)
        ubus.add(
            "callee_object",
            {
                "method1": {"method": handler1, "signature": {
                    "first": ubus.BLOBMSG_TYPE_INT32,
                }},
                "method2": {"method": handler2, "signature": {
                    "second": ubus.BLOBMSG_TYPE_INT32,
                }},
                "method3": {"method": handler3, "signature": {}},
            },
        )
        ubus.loop(500)

        assert results == {
            1: {'data': {'first': 1}, 'exits': True},
            2: {'data': {'second': 2}, 'exits': True},
            3: {'data': {}, 'exits': True},
        }

        ubus.disconnect()


def test_call_failed(ubusd_test, responsive_object, disconnect_after):
    path = UBUSD_TEST_SOCKET_PATH

    args1 = (
        [],
        ("responsive_object", ),
        ("responsive_object", "respond", ),
        ("responsive_object", "respond", 4, ),
        ("responsive_object", "respond", {"first": "test", "second": True, "third": 56}, "x"),
        ("responsive_object", "respond", {"first": "test", "second": True, "third": 56}, -1),
    )
    args2 = (
        ("responsive_object", "respond", {"first": 6, "second": True, "third": 56}, ),
        ("responsive_object", "respond", {"first": "test", "third": 56}, ),
        ("responsive_object", "respond", {"first": "test", "second": True, "third": 56, "x": 1}),
        ("responsive_object", "fail", {"first": "test", "second": True, "third": 56, "x": 1}),
        ("responsive_object", "fail", {}),
    )
    args3 = (
        ("responsive_object", "respond", {"first": "test", "second": True, "third": 56}, 2 ** 64),
    )
    with CheckRefCount(path, *(args1 + args2 + args3)):

        with pytest.raises(RuntimeError):
            ubus.objects(*args1[0])

        ubus.connect(path)
        for arg in args1:
            with pytest.raises(TypeError):
                ubus.call(*arg)

        for arg in args2:
            with pytest.raises(RuntimeError):
                ubus.call(*arg)

        for arg in args3:
            with pytest.raises(OverflowError):
                ubus.call(*arg)

        ubus.disconnect()
        del arg


def test_call(ubusd_test, responsive_object, disconnect_after):
    path = UBUSD_TEST_SOCKET_PATH

    ubus_object = "responsive_object"
    method1 = "respond"
    method2 = "multi_respond"
    data = {"first": "1", "second": False, "third": 22}

    with CheckRefCount(path, ubus_object, method1, method2):

        ubus.connect(socket_path=path)
        res = ubus.call(ubus_object, method1, data)
        assert len(res) == 1
        assert res[0] == {"first": "1", "second": False, "third": 22, "passed": True}

        res = ubus.call(ubus_object, method1, data, timeout=200)
        assert len(res) == 1
        assert res[0] == {"first": "1", "second": False, "third": 22, "passed": True}

        res = ubus.call(ubus_object, method2, {})
        assert len(res) == 3
        assert res[0] == {"passed1": True}
        assert res[1] == {"passed1": True, "passed2": True}
        assert res[2] == {"passed1": True, "passed2": True, "passed3": True}

        del res
        ubus.disconnect()


def test_call_max_min_number(ubusd_test, responsive_object, disconnect_after):
    path = UBUSD_TEST_SOCKET_PATH
    data1 = {"number": 2 ** 32}
    data2 = {"number": -(2 ** 32)}

    with CheckRefCount(path, data1, data2):

        ubus.connect(socket_path=path)
        res = ubus.call("responsive_object", "number", data1)
        assert res[0] == {"number": 2 ** 31 - 1, "passed": True}
        res = ubus.call("responsive_object", "number", data2)
        assert res[0] == {"number": -(2 ** 31), "passed": True}

        del res
        ubus.disconnect()


def test_multi_objects_listeners(ubusd_test, event_sender, calls_extensive, disconnect_after):
    counts = 20
    listen_test = {"pass%d" % e: False for e in range(counts)}
    object_test = {"pass%d" % e: False for e in range(counts)}
    event_name = "event_sender"
    timeout = 200

    path = UBUSD_TEST_SOCKET_PATH

    def passed_listen_gen(index):
        def passed(*args):
            listen_test["pass%d" % index] = True
        return passed

    def passed_object_gen(index):
        def passed(*args):
            object_test["pass%d" % index] = True
        return passed

    with CheckRefCount(path, time):

        for _ in range(5):
            ubus.connect(socket_path=path)

            for i in range(counts):
                ubus.listen((event_name, passed_listen_gen(i)))
                ubus.add(
                    "extensive_object_%d" % i,
                    {"method": {"method": passed_object_gen(i), "signature": {}}}
                )

            stored_counter = calls_extensive.counter.value
            while calls_extensive.counter.value - stored_counter < counts:
                ubus.loop(timeout)
            ubus.disconnect()

            for i in range(counts):
                current = "pass%d" % i
                assert listen_test[current]
                assert object_test[current]

            listen_test = {"pass%d" % e: False for e in range(counts)}
            object_test = {"pass%d" % e: False for e in range(counts)}


def test_unicode(ubusd_test, responsive_object, disconnect_after):
    path = UBUSD_TEST_SOCKET_PATH
    data = {"first": u"Příliš žluťoučký kůň úpěl ďábelské ódy.", "second": True, "third": 20}

    with CheckRefCount(path, data):

        ubus.connect(socket_path=path)
        res = ubus.call("responsive_object", "respond", data)
        assert res[0] == {
            "first": u"Příliš žluťoučký kůň úpěl ďábelské ódy.",
            "second": True, "third": 20, "passed": True
        }

        del res
        ubus.disconnect()
