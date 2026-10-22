import json, re
def load_layout(path):
    """Load a D2R layout JSON. The shipped files contain trailing commas, so strip them."""
    text = open(path, encoding='utf-8').read()
    text = re.sub(r',(\s*[\]}])', r'\1', text)
    return json.loads(text)
def walk(n, depth=0):
    yield n, depth
    for c in n.get('children') or []:
        yield from walk(c, depth+1)
def fields(n):
    return n.get('fields') or {}
