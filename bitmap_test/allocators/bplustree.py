"""
B+Tree implementation for bitmap block allocation
"""


class BPNode:
    """B+Tree node"""
    
    def __init__(self, order, is_leaf=True):
        self.order = order
        self.is_leaf = is_leaf
        self.keys = []
        self.children = []
        self.parent = None
        self.next = None
        self.prev = None
    
    def is_full(self):
        return len(self.keys) >= 2 * self.order - 1


class BitmapBPlusTree:
    """B+Tree indexed Bitmap Allocator"""
    
    def __init__(self, size, order=256):
        """Initialize bitmap with B+Tree indexing"""
        self.bitmap = [0] * size
        self.size = size
        self.order = order
        self.root = BPNode(order, is_leaf=True)
        self.first_leaf = self.root
        
        for i in range(size):
            self._insert_key(i)
    
    def find_free_block(self):
        """Find minimum free index"""
        leaf = self.first_leaf
        if leaf and leaf.keys:
            return leaf.keys[0]
        return -1
    
    def allocate_blocks(self, count):
        """Allocate count free blocks"""
        allocated = []
        
        try:
            for _ in range(count):
                idx = self.find_free_block()
                if idx == -1:
                    for allocated_idx in allocated:
                        self._insert_key(allocated_idx)
                        self.bitmap[allocated_idx] = 0
                    return []
                
                self.bitmap[idx] = 1
                self._delete_key(idx)
                allocated.append(idx)
            
            return sorted(allocated)
        except Exception:
            for allocated_idx in allocated:
                self._insert_key(allocated_idx)
                self.bitmap[allocated_idx] = 0
            return []
    
    def deallocate_blocks(self, indices):
        """Deallocate blocks"""
        try:
            for idx in indices:
                if self.bitmap[idx] == 0:
                    return False
                self.bitmap[idx] = 0
                self._insert_key(idx)
            return True
        except Exception:
            return False
    
    def count_free_blocks(self):
        """Count free blocks by traversing leaves"""
        count = 0
        leaf = self.first_leaf
        while leaf:
            count += len(leaf.keys)
            leaf = leaf.next
        return count
    
    # ===== B+Tree Operations =====
    
    def _insert_key(self, key):
        """Insert key into B+Tree"""
        if self.root.is_full():
            new_root = BPNode(self.order, is_leaf=False)
            new_root.children.append(self.root)
            self.root.parent = new_root
            self._split_child(new_root, 0)
            self.root = new_root
        
        self._insert_non_full(self.root, key)
    
    def _insert_non_full(self, node, key):
        """Insert into non-full node"""
        if node.is_leaf:
            i = 0
            while i < len(node.keys) and key > node.keys[i]:
                i += 1
            node.keys.insert(i, key)
        else:
            i = 0
            while i < len(node.keys) and key > node.keys[i]:
                i += 1
            
            child = node.children[i]
            if child.is_full():
                self._split_child(node, i)
                if key > node.keys[i]:
                    i += 1
                child = node.children[i]
            
            self._insert_non_full(child, key)
    
    def _split_child(self, parent, index):
        """Split full child"""
        full_child = parent.children[index]
        new_child = BPNode(self.order, full_child.is_leaf)
        
        mid_index = self.order - 1
        
        if full_child.is_leaf:
            new_child.keys = full_child.keys[mid_index:]
            full_child.keys = full_child.keys[:mid_index]
            
            new_child.next = full_child.next
            new_child.prev = full_child
            if full_child.next:
                full_child.next.prev = new_child
            full_child.next = new_child
            
            parent.keys.insert(index, new_child.keys[0])
        else:
            new_child.keys = full_child.keys[mid_index + 1:]
            new_child.children = full_child.children[mid_index + 1:]
            
            full_child.keys = full_child.keys[:mid_index]
            full_child.children = full_child.children[:mid_index + 1]
            
            for child in new_child.children:
                child.parent = new_child
            
            parent.keys.insert(index, full_child.keys[mid_index])
        
        parent.children.insert(index + 1, new_child)
        new_child.parent = parent
    
    def _delete_key(self, key):
        """Delete key from B+Tree"""
        self._delete_from_node(self.root, key)
        
        if not self.root.keys and self.root.children:
            self.root = self.root.children[0]
            self.root.parent = None
        
        curr = self.root
        while not curr.is_leaf:
            curr = curr.children[0]
        self.first_leaf = curr
    
    def _delete_from_node(self, node, key):
        """Delete key from node"""
        i = 0
        while i < len(node.keys) and key > node.keys[i]:
            i += 1
        
        if node.is_leaf:
            if i < len(node.keys) and node.keys[i] == key:
                node.keys.pop(i)
        else:
            if i < len(node.keys) and node.keys[i] == key:
                child = node.children[i]
                if len(child.keys) >= self.order:
                    predecessor = child.keys[-1]
                    node.keys[i] = predecessor
                    self._delete_from_node(child, predecessor)
                else:
                    sibling = node.children[i + 1]
                    child.keys.append(node.keys[i])
                    child.keys.extend(sibling.keys)
                    
                    if not child.is_leaf:
                        child.children.extend(sibling.children)
                        for c in sibling.children:
                            c.parent = child
                    else:
                        child.next = sibling.next
                        if sibling.next:
                            sibling.next.prev = child
                    
                    node.keys.pop(i)
                    node.children.pop(i + 1)
                    self._delete_from_node(child, key)
            else:
                child = node.children[i]
                if len(child.keys) < self.order:
                    self._fill(node, i)
                
                if i > len(node.keys):
                    self._delete_from_node(node.children[i - 1], key)
                else:
                    self._delete_from_node(node.children[i], key)
    
    def _fill(self, node, index):
        """Fill child with minimum keys"""
        if index > 0 and len(node.children[index - 1].keys) >= self.order:
            self._borrow_from_prev(node, index)
        elif index < len(node.children) - 1 and len(node.children[index + 1].keys) >= self.order:
            self._borrow_from_next(node, index)
        else:
            if index < len(node.children) - 1:
                self._merge(node, index)
            else:
                self._merge(node, index - 1)
    
    def _borrow_from_prev(self, node, child_index):
        """Borrow from previous sibling"""
        child = node.children[child_index]
        sibling = node.children[child_index - 1]
        
        child.keys.insert(0, sibling.keys.pop())
        
        if not child.is_leaf:
            child.children.insert(0, sibling.children.pop())
            child.children[0].parent = child
        
        node.keys[child_index - 1] = child.keys[0]
    
    def _borrow_from_next(self, node, child_index):
        """Borrow from next sibling"""
        child = node.children[child_index]
        sibling = node.children[child_index + 1]
        
        child.keys.append(sibling.keys.pop(0))
        
        if not child.is_leaf:
            child.children.append(sibling.children.pop(0))
            child.children[-1].parent = child
        
        node.keys[child_index] = sibling.keys[0]
    
    def _merge(self, node, index):
        """Merge child with sibling"""
        child = node.children[index]
        sibling = node.children[index + 1]
        
        if child.is_leaf:
            child.keys.extend(sibling.keys)
            child.next = sibling.next
            if sibling.next:
                sibling.next.prev = child
        else:
            child.keys.append(node.keys[index])
            child.keys.extend(sibling.keys)
            child.children.extend(sibling.children)
            for c in sibling.children:
                c.parent = child
        
        node.keys.pop(index)
        node.children.pop(index + 1)
