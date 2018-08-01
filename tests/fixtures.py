import os
from multiprocessing import Process, Value
import pytest
import subprocess
import time
import signal


UBUSD_TEST_SOCKET_PATH = "/tmp/ubus-test-socket"


class Guard(object):

    def __init__(self):
        self.counter = Value('i', 0)

    def __enter__(self):
        return self

    def __exit__(self, *args):
        return True

    def touch(self):
        self.counter.value += 1

    def wait(self):
        while not self.counter.value > 0:
            time.sleep(0.05)


@pytest.fixture(scope="session")
def ubusd_test():
    ubusd_instance = subprocess.Popen(["ubusd", "-s", UBUSD_TEST_SOCKET_PATH])
    while not os.path.exists(UBUSD_TEST_SOCKET_PATH):
        time.sleep(0.2)
    yield ubusd_instance
    ubusd_instance.kill()
    os.unlink(UBUSD_TEST_SOCKET_PATH)


@pytest.fixture(scope="function")
def event_sender():

    with Guard() as guard:

        def process_function():
            # connect to signal to properly terminate process
            global event_sender_proceed
            event_sender_proceed = True

            def exit(signum, frame):
                global event_sender_proceed
                event_sender_proceed = False
            signal.signal(signal.SIGTERM, exit)

            import ubus
            ubus.connect(UBUSD_TEST_SOCKET_PATH)

            while event_sender_proceed:
                ubus.send("event_sender", dict(a="b", c=3, d=False))
                guard.touch()
                time.sleep(0.1)
            ubus.disconnect()

        p = Process(target=process_function, name="event_sender")
        p.start()
        guard.wait()
        setattr(p, 'counter', guard.counter)

        yield p

        p.terminate()
        p.join()


@pytest.fixture(scope="function")
def registered_objects():
    with Guard() as guard:

        def process_function():

            def handler1(*arg):
                pass

            import ubus
            ubus.connect(UBUSD_TEST_SOCKET_PATH)
            ubus.add(
                "registered_object1",
                {
                    "method1": {"method": handler1, "signature": {}},
                    "method2": {"method": handler1, "signature": {
                        "first": ubus.BLOBMSG_TYPE_STRING,
                        "second": ubus.BLOBMSG_TYPE_BOOL,
                        "third": ubus.BLOBMSG_TYPE_INT32,
                    }},
                },
            )
            ubus.add(
                "registered_object2", {},
            )
            ubus.add(
                "registered_object3",
                {
                    "method1": {"method": handler1, "signature": {}},
                }
            )
            guard.touch()
            ubus.loop()

        p = Process(target=process_function)
        p.start()
        guard.wait()

        yield p

        p.terminate()
        p.join()


@pytest.fixture(scope="function")
def responsive_object():
    with Guard() as guard:

        def process_function():

            def handler1(handler, data):
                data["passed"] = True
                handler.reply(data)

            def handler2(handler, data):
                data["passed1"] = True
                handler.reply(data)
                data["passed2"] = True
                handler.reply(data)
                data["passed3"] = True
                handler.reply(data)

            def handler_fail(handler, data):
                raise Exception("Handler Fails")

            import ubus
            ubus.connect(UBUSD_TEST_SOCKET_PATH)
            ubus.add(
                "responsive_object",
                {
                    "respond": {"method": handler1, "signature": {
                        "first": ubus.BLOBMSG_TYPE_STRING,
                        "second": ubus.BLOBMSG_TYPE_BOOL,
                        "third": ubus.BLOBMSG_TYPE_INT32,
                    }},
                    "fail": {"method": handler_fail, "signature": {}},
                    "multi_respond": {"method": handler2, "signature": {}},
                    "number": {"method": handler1, "signature": {
                        "number": ubus.BLOBMSG_TYPE_INT32,
                    }},
                },
            )
            guard.touch()
            ubus.loop()

        p = Process(target=process_function)
        p.start()
        guard.wait()

        yield p

        p.terminate()
        p.join()


@pytest.fixture(scope="function")
def call_for_object():
    with Guard() as guard:

        def process_function():

            # connect to signal to properly terminate process
            global call_for_object_proceed
            call_for_object_proceed = True

            def exit(signum, frame):
                global call_for_object_proceed
                call_for_object_proceed = False
            signal.signal(signal.SIGTERM, exit)

            import ubus
            ubus.connect(UBUSD_TEST_SOCKET_PATH)
            args = [
                ('callee_object', 'method1', {'first': 1}),
                ('callee_object', 'method2', {'second': 2}),
                ('callee_object', 'method3', {}),
            ]
            while call_for_object_proceed:
                for arg in args:
                    try:
                        ubus.call(*arg)
                    except RuntimeError as e:
                        pass
                    guard.touch()
                    time.sleep(0.05)

        p = Process(target=process_function)
        p.start()
        guard.wait()

        yield p

        p.terminate()
        p.join()


@pytest.fixture(scope="function")
def calls_extensive():
    with Guard() as guard:

        def process_function():

            # connect to signal to properly terminate process
            global calls_extensive_proceed
            calls_extensive_proceed = True

            def exit(signum, frame):
                global calls_extensive_proceed
                calls_extensive_proceed = False
            signal.signal(signal.SIGTERM, exit)

            import ubus
            ubus.connect(UBUSD_TEST_SOCKET_PATH)
            while calls_extensive_proceed:
                for i in range(20):
                    try:
                        ubus.call('extensive_object_%d' % i, 'method', {})
                        time.sleep(0.1)
                    except RuntimeError as e:
                        pass
                    guard.touch()

        p = Process(target=process_function)
        p.start()
        guard.wait()
        setattr(p, 'counter', guard.counter)

        yield p

        p.terminate()
        p.join()


@pytest.fixture(scope="function")
def disconnect_after():
    yield None
    import ubus
    try:
        ubus.disconnect()
    except:
        pass
