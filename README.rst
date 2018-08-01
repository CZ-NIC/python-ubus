Python bindings for ubus
========================
Code in this directory enables a subset of libubus functions to be used directly from python.

Installation
------------

To install these bidning you need to have the following libraries and headers installed:

* libubox
* libubus
* python/python3


Examples
########

connect and disconnect
----------------------
To connect you need to::

    import ubus
    ubus.connect("/var/run/ubus.sock")

To disconnect you can simply::

    ubus.disconnect()

Note that calling connect()/disconnect() on opened/closed connection will throw an exception.

add
---
To add an object to ubus you can (you need to become root first)::

    def callback(handler, data):
        handler.reply(data)  # this should return exactly the same data to the caller

    ubus.add(
        "my_object", {
            "my_method": {"method": callback, "signature": {
                    "first": ubus.BLOBMSG_TYPE_STRING,
                    "second": ubus.BLOBMSG_TYPE_BOOL,
                    "third": ubus.BLOBMSG_TYPE_INT32,
            }},
         },
    )

You need to enter the loop to serve the object methods afterwards::

    ubus.loop()


Note that it might not be a good idea to call the callback function recursively.


objects
-------
To list the objects which are currently connected to ubus you can call::

    ubus.objects()

    ->

    {u'my_object': {u'my_method': {u'first': 3, u'second': 7, u'third': 5}}}



call
----
To call an actual method on an object you can use::

    ubus.call("my_object", "my_method", {"first": "my_string", "second": True, "third": 42})

    ->

    [{"first": "my_string", "second": True, "third": 42}]


listen
------
To listen for an event you can::

    def callback(event, data):
        print(event, data)  # just print event name and data to stdout

    ubus.listen(("my_event", callback))

And you need to enter the loop to start to listen::

    ubus.loop()

Note that it might not be a good idea to call the callback function recursively.

send
----
This will send an event to ubus::

    ubus.send("my_event", {"some": "data"})


Notes
#####

There are some tests present ('tests/' directory). So feel free to check it for some more complex examples.
To run the tests you need to have ubus installed and become root::

    sudo python setup.py test
