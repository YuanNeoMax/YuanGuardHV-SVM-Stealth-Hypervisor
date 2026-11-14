with open('C:/Program Files (x86)/Windows Kits/10/Lib/10.0.26100.0/km/x64/ntoskrnl.lib', 'rb') as f:
    data = f.read()
needle = b'__imp_PsSuspendProcess'
idx = data.find(needle)
print('__imp_PsSuspendProcess at:', hex(idx) if idx >= 0 else 'NOT FOUND')
needle2 = b'PsSuspendProcess'
idx2 = data.find(needle2)
print('PsSuspendProcess at:', hex(idx2) if idx2 >= 0 else 'NOT FOUND')
print('file size:', len(data))
