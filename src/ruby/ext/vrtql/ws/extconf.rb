require 'mkmf'

# Define a variable to hold the OS-specific define
$os_define = ''

# Check for specific operating systems
case RbConfig::CONFIG['host_os']
when /linux/
  $os_define = '__LINUX__'
when /freebsd/
  $os_define = '__FREEBSD__'
when /netbsd/
  $os_define = '__NETBSD__'
when /openbsd/
  $os_define = '__OPENBSD__'
when /sunos|solaris/
  $os_define = '__SOLARIS__'
when /mswin|mingw/
  $os_define = '__WINDOWS__'

  # Check for the winsock2 library
  unless have_library('ws2_32')
    raise 'Could not find required library ws2_32'
  end
else
  # Default configuration for other platforms
  $os_define = '__OTHER_OS__'
  warn "Unrecognized OS: #{RbConfig::CONFIG['host_os']}"
end

unless have_library('ssl')
  abort "OpenSSL library is missing. Please install it."
end

unless have_library('crypto')
  abort "Crypto library is missing. Please install it."
end

unless $os_define.include?('__WINDOWS__')
  unless have_library('pthread')
    abort "Pthread library is missing. Please install it."
  end
end

# Create the platform-specific header file
File.open('platform.h', 'w') do |file|
  file.puts "#ifndef PLATFORM_H"
  file.puts "#define PLATFORM_H"
  file.puts
  file.puts "#define #{ $os_define }"
  file.puts
  file.puts "#endif /* PLATFORM_H */"
end

vrtql_files = [
  'vrtql/http_message.c',
  'vrtql/http_parser.c',
  'vrtql/message.c',
  'vrtql/socket.c',
  'vrtql/vrtql.c',
  'vrtql/websocket.c'
]

source_files = vrtql_files +
               Dir.glob('*.c') +
               Dir.glob('vrtql/util/*.c') +
               Dir.glob('vrtql/mpack/*.c')

# Use the base name (without extension) for object files
obj_files = source_files.map { |f| f.gsub('.c', '.o') }

# This is important. The $objs array tells mkmf what object files need to be
# linked to form the shared library.
$objs = obj_files

# Get paths to include directories
include_dirs = ['.', './vrtql', './vrtql/util', './vrtql/mpack']

# Create a string of -I directives
incflags = include_dirs.map { |dir| "-I#{dir}" }.join(' ')

project_flags = " -DMG_ENABLE_LINES -DMG_ENABLE_OPENSSL"

# Add the -I directives to CFLAGS and INCFLAGS
$CFLAGS   << " #{incflags}" << project_flags
$INCFLAGS << " #{incflags}"

# Proceed with the usual configuration and compilation steps
create_makefile('vrtql/ws')

# Modify the Makefile to build object files in their respective directories
File.open('Makefile', 'a') do |f|
  f.puts
  f.puts "\n.PHONY: clean_o_files\n"
  f.puts "install: clean_o_files\n"
  f.puts "clean_o_files:\n\t"
  f.puts "\t$(RM) #{obj_files.join(' ')}\n"
  f.puts "\t$(RM) $(TIMESTAMP_DIR)/ws.so\n"
  f.puts "\t$(RM) $(TIMESTAMP_DIR)/*.c\n"
  f.puts "\t$(RM) $(TIMESTAMP_DIR)/*.h\n"
  f.puts "\t$(RM) $(TIMESTAMP_DIR)/Makefile\n"
  f.puts "\t$(RM) $(TIMESTAMP_DIR)/extconf.rb\n"
  f.puts "\t$(RM) -rf $(TIMESTAMP_DIR)/vrtql\n"
  f.puts

  obj_files.zip(source_files).each do |obj_file, source_file|
    f.puts "#{obj_file}: #{source_file}"
    f.puts "\t$(CC) $(INCFLAGS) $(CPPFLAGS) $(CFLAGS) $(COUTFLAG)$@ -c #{source_file}"
  end
end
