#include "VoyagerItems.h"
#include "VoyagerGameMode.h"
#include "Engine/World.h"

int32 AVoyagerPlayerState::ItemCount(EVoyagerItem Item) const
{return Items.IsValidIndex(int32(Item))?Items[int32(Item)]:0;}
bool AVoyagerPlayerState::AddItem(EVoyagerItem Item,int32 Quantity)
{
    const int32 Index=int32(Item);
    if(!HasAuthority()||Index<0||Index>=int32(EVoyagerItem::Count)||Quantity<=0||Quantity>999||ItemCount(Item)>999-Quantity)return false;
    Items.SetNum(int32(EVoyagerItem::Count));Items[Index]+=Quantity;ForceNetUpdate();
    if(auto GM=GetWorld()->GetAuthGameMode<AVoyagerGameMode>())GM->SaveExpedition();return true;
}
bool AVoyagerPlayerState::TakeItem(EVoyagerItem Item,int32 Quantity)
{
    if(!HasAuthority()||Quantity<=0||ItemCount(Item)<Quantity)return false;
    Items[int32(Item)]-=Quantity;ForceNetUpdate();
    if(auto GM=GetWorld()->GetAuthGameMode<AVoyagerGameMode>())GM->SaveExpedition();return true;
}
bool AVoyagerPlayerState::GrantHuntLoot(int32 Meat,int32 Hide,int32 Bone)
{
    if(!HasAuthority()||Meat<0||Hide<0||Bone<0||Meat>20||Hide>20||Bone>20||
        ItemCount(EVoyagerItem::RawMeat)>999-Meat||ItemCount(EVoyagerItem::Hide)>999-Hide||ItemCount(EVoyagerItem::Bone)>999-Bone)return false;
    Items.SetNum(int32(EVoyagerItem::Count));Items[int32(EVoyagerItem::RawMeat)]+=Meat;
    Items[int32(EVoyagerItem::Hide)]+=Hide;Items[int32(EVoyagerItem::Bone)]+=Bone;ForceNetUpdate();
    if(auto GM=GetWorld()->GetAuthGameMode<AVoyagerGameMode>())GM->SaveExpedition();return true;
}
