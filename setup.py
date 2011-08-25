import sys
from distutils.core import setup, Extension

extra_compile_args = ['-I./fastlz/', '-fPIC', '--std=gnu99', '-Wall', '-g', '-D_GNU_SOURCE']
extra_link_args = ['-L./fastlz/', '-static', '-lfastlz']

setup(
	name = 'quicklz',
	version = '1.0.0',
	maintainer = 'Fu Haiping',
	maintainer_email = 'haipingf@gmail.com',
	url = 'http://code.google.com/p/py-fastlz/',

	classifiers = [
		'Development Status :: 4 - Beta',
		'Environment :: Other Environment',
		'Intended Audience :: Developers',
		'License :: OSI Approved :: GNU Library or Lesser General Public License (LGPL)',
		'Operating System :: POSIX',
		'Programming Language :: C',
		'Programming Language :: Python',
		'Programming Language :: Python :: 2.4',
		'Programming Language :: Python :: 2.5',
		'Programming Language :: Python :: 2.6',
		'Programming Language :: Python :: 2.7',
		'Topic :: Compression',
		'Topic :: Software Development :: Libraries'
	],

	description = 'Python bindings for fastlz compression library',

	packages = ['fastlz'],
	package_dir = {'fastlz': ''},

	ext_modules = [
		Extension('fastlz',
			sources = [
				# python stuff
				'fastlz-module.c',
			],
			extra_compile_args = extra_compile_args,
			extra_link_args = extra_link_args
		)
	]
)
