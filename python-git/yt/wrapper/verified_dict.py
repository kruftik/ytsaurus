import collections

class VerifiedDict(collections.MutableMapping):
    def __init__(self, keys_to_ignore, transform_func, *args, **kwargs):
        self._enable_check = False
        self._make_subdicts_verified = True
        self._keys_to_ignore = keys_to_ignore
        self._transform_func = transform_func

        self.store = dict()
        self.update(dict(*args, **kwargs))

        self._enable_check = True
        self._make_subdicts_verified = False

    def __getitem__(self, key):
        return self.store[key]
    
    def __setitem__(self, key, value):
        if self._make_subdicts_verified and isinstance(value, dict) and key not in self._keys_to_ignore:
            value = VerifiedDict([], self._transform_func, value)
        if self._enable_check and key not in self.store:
            raise RuntimeError("Set failed: %r key is missing" % key)
        if self._transform_func is None:
            self.store[key] = value
        else:
            self.store[key] = self._transform_func(value)
    
    def __delitem__(self, key):
        del self.store[key]
    
    def __contains__(self, key):
        return key in self.store
    
    def __iter__(self):
        return iter(self.store)
    
    def __len__(self):
        return len(self.store)

