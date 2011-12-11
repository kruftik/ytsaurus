#!/usr/bin/python
from cfglib.ytremote import *
from cfglib.ytwin import *
import cfglib.opts as opts

build_dir = r'c:\Users\Max\Work\Yandex\YT\build'

Logging = {
        'Writers' : [
                {
                        'Name' : "File",
                        'Type' : "File",
                        'FileName' : "%(log_path)s",
                        'Pattern' : "$(datetime) $(level) $(category) $(message)"
                },
                {
                        'Name' : "StdErr",
                        'Type' : "StdErr",
                        'Pattern' : "$(datetime) $(level) $(category) $(message)"
                }
        ],
        'Rules' : [
                { 
                        'Categories' : [ "*" ], 
                        'MinLevel' : "Debug", 
                        'Writers' : [ "File" ] 
                },
                {
                        'Categories' : [ "*" ], 
                        'MinLevel' : "Info", 
                        'Writers' : [ "StdErr" ] 
                }
        ]
}

MasterAddresses = opts.limit_iter('--masters',
        ['localhost:%d' % port for port in xrange(8001, 8004)])

class Base(AggrBase):
        path = opts.get_string('--name', 'control')
        
class Server(Base):
        bin_path = os.path.join(build_dir, r'bin\Debug\server.exe')
                
class Master(WinNode, Server):
        address = Subclass(MasterAddresses)
        params = Template('--cell-master --config %(config_path)s --old_config %(old_config_path)s --port %(port)d --id %(__name__)s')

        config = Template({
                'meta_state' : {
                                'cell' : {
                                'addresses' : MasterAddresses
                        },
                        'snapshot_path' : r'%(work_dir)s\snapshots',
                        'log_path' : r'%(work_dir)s\logs',
                },                      
                'Logging' : Logging
        })
        
        def run(cls, fd):
                print >>fd, 'mkdir %s' % cls.config['meta_state']['snapshot_path']
                print >>fd, 'mkdir %s' % cls.config['meta_state']['log_path']
                print >>fd, cls.run_tmpl
                
        def clean(cls, fd):
                print >>fd, 'del %s' % cls.log_path
                print >>fd, r'del /Q %s\*' % cls.config['meta_state']['snapshot_path']
                print >>fd, r'del /Q %s\*' % cls.config['meta_state']['log_path']
        
        
class Holder(WinNode, Server):
        address = Subclass(opts.limit_iter('--holders',
                        [('localhost:%d' % p) for p in range(9000, 9100)]))
        
        params = Template('--chunk-holder --config %(config_path)s --old_config %(old_config_path)s --port %(port)d')
        
        config = Template({ 
                'masters' : { 'addresses' : MasterAddresses },
                'storage_locations' : [
                    { 'path' : r'%(work_dir)s\chunk_storage.0' }
                ],
                'cache_location' : {
                    'path' : r'%(work_dir)s\chunk_cache'
                },
                'cache_remote_reader' : { },
                'cache_sequential_reader' : { },
                'logging' : Logging
        })
        
        def clean(cls, fd):
                print >>fd, 'del %s' % cls.log_path
                for location in cls.config['storage_locations']:
                        print >>fd, 'rmdir /S /Q   %s' % location['path']
                print >>fd, 'rmdir /S /Q   %s' % cls.config['cache_location']['path']


class Client(WinNode, Base):
    bin_path = os.path.join(build_dir, r'bin\Debug\send_chunk.exe') 

    params = Template('--config %(config_path)s')

    config_base = { 
        'ReplicationFactor' : 2,
        'BlockSize' : 2 ** 20,

        'ThreadPool' : {
            'PoolSize' : 1,
            'TaskCount' : 1
        },

        'Logging' : Logging,

        'Masters' : { 
            'Addresses' : MasterAddresses 
        },

        'ChunkWriter' : {
            'WindowSize' : 40,
            'GroupSize' : 8 * (2 ** 20)
        } 
    }

    def clean(cls, fd):
        print >>fd, 'del %s' % cls.log_path

def client_config(d):
    d.update(Client.config_base)
    return d

class PlainChunk(Client):
    config = Template(client_config({
        'YPath' : '/files/plain_chunk',
        'Input' : {
            'Type' : 'zero', # stream of zeros
            'Size' : (2 ** 20) * 256 
        }
    }))

class TableChunk(Client):
    config = Template(client_config({ 
        'Schema' : '[["berlin"; "madrid"; ["xxx"; "zzz"]]; ["london"; "paris"; ["b"; "m"]]]',
        'YPath' : '/files/table_chunk',
        'Input' : {
            'Type' : 'random_table',
            'Size' : (2 ** 20) * 20 
        }
    }))

class TableChunkSequence(Client):
    config = Template(client_config({ 
        'Schema' : '[["berlin"; "madrid"; ["xxx"; "zzz"]]; ["london"; "paris"; ["b"; "m"]]]',
        'ChunkSize' : (2 ** 20) * 7,
        'YPath' : '/table',
        'Input' : {
            'Type' : 'random_table',
            'Size' : (2 ** 20) * 20 
        }
    }))

configure(Base)
