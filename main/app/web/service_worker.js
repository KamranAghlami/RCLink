const cache_name = 'cache-v0.0.0';

self.addEventListener('install', function (event) {
    event.waitUntil(
        caches.open(cache_name).then(function (cache) {
            return cache.addAll([]);
        })
    );
});

self.addEventListener('activate', function (event) {
    event.waitUntil(
        caches.keys().then(function (cache_name) {
            return Promise.all(
                cache_name.map(function (name) {
                    if (name !== cache_name)
                        return caches.delete(name);
                })
            );
        })
    );
});

self.addEventListener('fetch', function (event) {
    event.respondWith(
        caches.match(event.request).then(function (response) {
            return response || fetch(event.request).then(function (fetch_response) {
                return caches.open(cache_name).then(function (cache) {
                    cache.put(event.request, fetch_response.clone());

                    return fetch_response;
                });
            });
        })
    );
});
