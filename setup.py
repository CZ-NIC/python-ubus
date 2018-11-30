#
# python-ubus - python bindings for ubus
#
# Copyright (C) 2017-2018 Stepan Henek <stepan.henek@nic.cz>
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU Lesser General Public License version 2.1
# as published by the Free Software Foundation
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#


from setuptools import setup, Extension

extension = Extension(
    'ubus',
    ['./ubus_python.c'],
    libraries=['ubus', 'blobmsg_json', 'ubox'],
)

setup(
    name='ubus',
    version='0.1.1',
    author="Stepan Henek",
    url="https://gitlab.labs.nic.cz/turris/python-ubus",
    description="Python bindings for libubus",
    long_description=open("README.rst").read(),
    ext_modules=[extension],
    provides=['ubus'],
    license="LGPL 2.1",
    setup_requires=['pytest-runner'],
    tests_require=['pytest'],
    zip_safe=False,
    classifiers=[
        'Programming Language :: Python :: 2.7',
        'Programming Language :: Python :: 3',
        'Development Status :: 5 - Production/Stable',
        'Intended Audience :: Developers',
        'License :: OSI Approved :: GNU Lesser General Public License v2 or later (LGPLv2+)',
        'Operating System :: POSIX :: Linux',
        'Topic :: Software Development :: Libraries',
        'Topic :: System :: Systems Administration',
    ]
)
