"""Shared extraction for host tests of production net functions and wrappers."""
def function(source, name):
    import re
    match = re.search(r'^(?:static )?(?:int|void) ' + re.escape(name) + r'\([^;]*?\)\s*\{', source, re.MULTILINE)
    if not match:
        raise AssertionError(f'missing production function {name}')
    opening = source.index('{', match.start())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def owned_function(source, name):
    """Retain public worker ownership wrapper as well as its actual body."""
    owned = name + '_owned'
    return function(source, owned) + '\n' + function(source, name)


CONTROL_STUB = '''
/* These association tests exercise the owned body with an available lease.
 * Rejected leases and nesting are tested by test_net_offline.py. */
static int control_enter(void){return 1;}
static void control_leave(void){}
'''
