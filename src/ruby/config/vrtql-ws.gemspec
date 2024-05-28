r_files   = Dir.glob('lib/vrtql/ws/*.rb')
c_files   = Dir.glob('ext/vrtql/ws/*.c')
c_files  += Dir.glob('ext/vrtql/ws/*.h')
c_files  += Dir.glob('ext/vrtql/ws/vrtql/*.c')
c_files  += Dir.glob('ext/vrtql/ws/vrtql/*.h')
c_files  += Dir.glob('ext/vrtql/ws/vrtql/util/*')
c_files  += Dir.glob('ext/vrtql/ws/vrtql/mpack/*')

Gem::Specification.new do |s|
  s.name          = 'vrtql-ws'
  s.version       = '2.0.0'
  s.authors       = ['VRTQL']
  s.email         = ['dev@vrtql.com']
  s.summary       = 'VRTQL websocket library'
  s.description   = 'A Ruby websocket library'
  s.homepage      = 'https://github.com/vrtql/websockets'
  s.license       = 'MIT'
  s.files         = %w[
    MIT-LICENSE
  ] + c_files + r_files + ["README.md"]
  s.require_paths = ['lib']
  s.extensions    = Dir.glob('ext/**/extconf.rb')

  # Specify that RDoc should be used to generate documentation
  s.extra_rdoc_files = Dir.glob('doc/vrtql.rb') + ["README.md"]

  # Specify the files to be used to generate RDoc
  s.rdoc_options = ['--main', 'README.md']
end
