"""
B-Tree implementation for bitmap block allocation
"""

import bisect
from core import BaseAllocator, register


class BNode:
    """B-Tree node"""
    
    def __init__(self, order, is_leaf=True):
        self.order = order
        self.is_leaf = is_leaf
        self.keys = []
        self.children = []
        self.parent = None
    
    def is_full(self):
        return len(self.keys) >= 2 * self.order - 1


@register("btree", display_name="B-Tree")
class BitmapBTree(BaseAllocator):
    """B-Tree indexed Bitmap Allocator"""
    
    def __init__(self, size, order=256):
        """Initialize bitmap with B-Tree indexing"""
        super().__init__(size)
        self.order = order
        self.root = BNode(order, is_leaf=True)
        self.next_fresh = 0
    
    def find_free_block(self):
        """Find minimum free index among recycled B-Tree and fresh range."""
        tree_min = -1
        if self.root and self.root.keys:
            node = self.root
            while not node.is_leaf:
                node = node.children[0]
            tree_min = node.keys[0] if node.keys else -1
        fresh_min = self.next_fresh if self.next_fresh < self.size else -1
        if tree_min == -1:
            return fresh_min
        if fresh_min == -1:
            return tree_min
        return min(tree_min, fresh_min)
    
    def allocate_blocks(self, count):
        """Allocate count free blocks"""
        if count <= 0:
            return []
        if self.free_blocks_remaining < count:
            return []

        allocated = []

        for _ in range(count):
            idx = self.find_free_block()
            if idx == -1:
                return []

            self.bitmap[idx] = 1
            if idx == self.next_fresh:
                self.next_fresh += 1
            else:
                self._delete_key(idx)
            allocated.append(idx)

        return allocated
    
    def deallocate_blocks(self, indices):
        """Deallocate blocks"""
        if not self.validate_deallocation(indices):
            return False

        for idx in indices:
            self.bitmap[idx] = 0
            self._insert_key(idx)
        return True
    
    def count_free_blocks(self):
        """Count free blocks"""
        return self.free_blocks_remaining
    
    # ===== B-Tree Operations =====
    
    def _insert_key(self, key):
        """Insert key into B-Tree"""
        if self.root.is_full():
            new_root = BNode(self.order, is_leaf=False)
            new_root.children.append(self.root)
            self.root.parent = new_root
            self._split_child(new_root, 0)
            self.root = new_root
        
        self._insert_non_full(self.root, key)
    
    def _insert_non_full(self, node, key):
        """Insert key into non-full node"""
        if node.is_leaf:
            i = bisect.bisect_left(node.keys, key)
            node.keys.insert(i, key)
        else:
            i = bisect.bisect_left(node.keys, key)

            child = node.children[i]
            if child.is_full():
                self._split_child(node, i)
                if key > node.keys[i]:
                    i += 1

            self._insert_non_full(node.children[i], key)
    
    def _split_child(self, parent, index):
        """Split full child"""
        full_child = parent.children[index]
        new_child = BNode(self.order, full_child.is_leaf)
        
        mid_index = self.order - 1
        mid_key = full_child.keys[mid_index]
        
        new_child.keys = full_child.keys[mid_index + 1:]
        full_child.keys = full_child.keys[:mid_index]
        
        if not full_child.is_leaf:
            new_child.children = full_child.children[mid_index + 1:]
            full_child.children = full_child.children[:mid_index + 1]
        
        parent.keys.insert(index, mid_key)
        parent.children.insert(index + 1, new_child)
    
    def _delete_key(self, key):
        """Delete key from B-Tree"""
        self._delete_from_node(self.root, key)
        
        if not self.root.keys and self.root.children:
            self.root = self.root.children[0]
    
    def _delete_from_node(self, node, key):
        """Delete key from node"""
        i = bisect.bisect_left(node.keys, key)
        
        if i < len(node.keys) and key == node.keys[i]:
            if node.is_leaf:
                node.keys.pop(i)
            else:
                self._delete_internal_node(node, key, i)
        elif not node.is_leaf:
            is_in_subtree = (i == len(node.keys))
            
            if len(node.children[i].keys) < self.order:
                self._fill(node, i)
            
            if is_in_subtree and i > len(node.keys):
                self._delete_from_node(node.children[i - 1], key)
            else:
                self._delete_from_node(node.children[i], key)
    
    def _delete_internal_node(self, node, key, index):
        """Delete from internal node"""
        key_to_delete = node.keys[index]
        
        if len(node.children[index].keys) >= self.order:
            predecessor = self._get_predecessor(node, index)
            node.keys[index] = predecessor
            self._delete_from_node(node.children[index], predecessor)
        elif len(node.children[index + 1].keys) >= self.order:
            successor = self._get_successor(node, index)
            node.keys[index] = successor
            self._delete_from_node(node.children[index + 1], successor)
        else:
            self._merge(node, index)
            self._delete_from_node(node.children[index], key_to_delete)
    
    def _get_predecessor(self, node, index):
        """Get predecessor key"""
        curr = node.children[index]
        while not curr.is_leaf:
            curr = curr.children[-1]
        return curr.keys[-1]
    
    def _get_successor(self, node, index):
        """Get successor key"""
        curr = node.children[index + 1]
        while not curr.is_leaf:
            curr = curr.children[0]
        return curr.keys[0]
    
    def _fill(self, node, index):
        """Fill child with fewer keys"""
        if index != 0 and len(node.children[index - 1].keys) >= self.order:
            self._borrow_from_prev(node, index)
        elif index != len(node.children) - 1 and len(node.children[index + 1].keys) >= self.order:
            self._borrow_from_next(node, index)
        else:
            if index != len(node.children) - 1:
                self._merge(node, index)
            else:
                self._merge(node, index - 1)
    
    def _borrow_from_prev(self, node, child_index):
        """Borrow from previous sibling"""
        child = node.children[child_index]
        sibling = node.children[child_index - 1]
        
        child.keys.insert(0, node.keys[child_index - 1])
        node.keys[child_index - 1] = sibling.keys.pop()
        
        if not child.is_leaf:
            child.children.insert(0, sibling.children.pop())
    
    def _borrow_from_next(self, node, child_index):
        """Borrow from next sibling"""
        child = node.children[child_index]
        sibling = node.children[child_index + 1]
        
        child.keys.append(node.keys[child_index])
        node.keys[child_index] = sibling.keys.pop(0)
        
        if not child.is_leaf:
            child.children.append(sibling.children.pop(0))
    
    def _merge(self, node, index):
        """Merge child with sibling"""
        child = node.children[index]
        sibling = node.children[index + 1]

        child.keys.append(node.keys.pop(index))
        child.keys.extend(sibling.keys)

        if not child.is_leaf:
            child.children.extend(sibling.children)

        node.children.pop(index + 1)
