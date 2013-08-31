
from operator import itemgetter

import centidb
import centidb.encoders


def open_store():
    store = centidb.open('LmdbEngine',
        path='/home/dmw/i2.lmdb',
         map_size=1048576*1024*10,
         map_async=True,
         sync=False,
         metasync=False,
         writemap=True)

    store.add_collection('posts')
    encoder = centidb.encoders.make_json_encoder(sort_keys=True)

    reddits = store.add_collection('reddits', encoder=encoder,
        key_func=itemgetter('id'))
    reddits.add_index('links', itemgetter('links'))
    reddits.add_index('comments', itemgetter('comments'))

    links = store.add_collection('links', encoder=encoder,
        key_func=itemgetter('id'))
    links.add_index('comments', itemgetter('comments'))

    comments = store.add_collection('comments', encoder=encoder,
        key_func=itemgetter('id'))
    comments.add_index('author', itemgetter('author', 'created'))
    comments.add_index('subreddit', itemgetter('subreddit_id', 'created'))

    store.add_collection('users', encoder=encoder,
        key_func=itemgetter('username'))
    store.add_collection('digits')

    return store

