import json

nodes = {
    '0': 'node-0',
    '1': 'node-1',
    '2': 'node-2',
    '3': 'node-3',
    '4': 'node-4',
    '5': 'node-5',
    '6': 'node-6',
    '7': 'node-7',
    '8': 'node-8',
    '9': 'node-9',
}

# (from, to)
edges =[
    ('0-0', '3-0'),
    ('1-0', '3-1'),
    ('2-0', '4-0'), 
    ('3-2', '5-0'),
    ('4-1', '5-1'),
    ('5-2', '6-0'),
    ('6-1', '7-0'),
    ('6-2', '8-0'),
    ('6-3', '9-0')
]

json_content = {
    "nodes": [],
    "edges": [],
    "timeLimit": -404
}

for node in nodes.keys():
    json_content["nodes"].append({
        "id": nodes[node]
    })  

for edge in edges:
    edge_dict = {
        "source": nodes[edge[0].split('-')[0]],
        "target": nodes[edge[1].split('-')[0]],
        "sTimeline": [],
        "tTimeline": []
    }
    for i in [0, 1]:
        with open(f"results/example_congest/queue-{edge[i]}.dat") as f:
            rows = f.readlines()
            for row in rows:
                data = row.split()
                time, queue = data[0], data[1]
                if i == 0:
                    edge_dict['sTimeline'].append((time, queue))
                else:
                    edge_dict['tTimeline'].append((time, queue))
        json_content['timeLimit'] = max(json_content['timeLimit'], len(rows))        
    json_content['edges'].append(edge_dict)

json_str = json.dumps(json_content)

with open("results/data.json", 'w') as f:
    f.write(json_str)

